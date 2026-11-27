#include "audio_system.h"

#include <algorithm>
#include <cstring>

#include "ecs/scene_evaluation.h"
#include "media/timeline_audio_source.h"
#include "support/audio_effects.h"
#include "support/media_time.h"

namespace huxerui::avkit::detail {
namespace {

using media::AudioFrame;
using media::AudioFormat;

struct ClipEnvelope {
  std::int64_t begin, end;
  std::int64_t fade_in, fade_out;
  float gain;
  bool robot;

  double Volume(std::int64_t frame, std::uint32_t rate) const {
    const auto in = fade_in ? std::min(1.0, double(frame - begin) / fade_in) : 1.0;
    const auto out = fade_out ? std::min(1.0, double(end - frame - 1) / fade_out) : 1.0;
    const auto effect = robot ? RobotVoiceGain(frame - begin, rate) : 1.0;
    return gain * std::min(in, out) * effect;
  }
};

bool Robot(entityx::Entity entity) {
  const auto effect = entity.component<VoiceEffect>();
  return effect && *effect == VoiceEffect::Robot;
}

void Mix(std::span<std::int64_t> out, std::span<const std::byte> bytes, const AudioFormat& format,
         std::int64_t at, const ClipEnvelope& envelope, bool reverse) {
  const auto channels = format.ChannelCount();
  const auto frames = bytes.size() / (channels * sizeof(std::int16_t));
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto gain = envelope.Volume(at + frame, format.sample_rate);
    const auto source = reverse ? frames - 1 - frame : frame;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const auto offset = (source * channels + channel) * sizeof(std::int16_t);
      std::int16_t value;
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
      out[frame * channels + channel] += std::lround(value * gain);
    }
  }
}

} // namespace

void AudioSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  auto block = root.component<AudioBlockEvaluation>();
  const auto sources = root.component<const TimelineServices>();
  const auto& format = block->format;
  const auto start = ExactTicks(block->start, format.sample_rate), end = start + block->samples;
  const auto channels = format.ChannelCount();
  const auto live = entities.entities_with_components<LivePcmInput>();
  if (!sources && live.begin() != live.end()) {
    auto first = *live.begin();
    auto next = live.begin();
    ++next;
    if (next == live.end() && !Robot(first)) {
      const auto& pcm = first.component<LivePcmInput>()->frame;
      if (pcm.pts == block->start && pcm.sample_count == block->samples && pcm.format == format) {
        block->output = pcm;
        return;
      }
    }
  }

  std::vector<std::int64_t> sum(std::size_t(block->samples) * channels);
  if (sources) {
    entities.each<AudioSource, ClipTiming, AudioMix>([&](entityx::Entity clip, const AudioSource& binding,
                                                         const ClipTiming& timing, const AudioMix& mix) {
      const auto& placement = timing;
      const auto begin = AtScale(placement.timeline.start, format.sample_rate).ticks;
      const auto finish = AtScale(placement.timeline.end, format.sample_rate).ticks;
      const auto from = std::max(start, begin), to = std::min(end, finish);
      if (from >= to || mix.muted || !mix.gain) return;
      const auto& movie = sources->movie(binding.asset);
      const auto source = Add(placement.source.range.start, binding.source_offset);
      const auto source_end = Add(placement.source.range.end, binding.source_offset);
      ValidateMediaTrack({binding.asset, binding.track, {source, source_end}}, movie, media::TrackKind::Audio);
      const auto trim = AtScale(source, movie.pcm.sample_rate).ticks;
      const auto length = AtScale(source_end, movie.pcm.sample_rate).ticks - trim;
      if (length <= 0) return;
      const ClipEnvelope envelope{begin, finish, AtScale(mix.fade_in, format.sample_rate).ticks,
                                  AtScale(mix.fade_out, format.sample_rate).ticks, mix.gain, Robot(clip)};
      auto at = from;
      sources->audio.Read(clip, binding, movie, format, trim, length, finish - begin,
          placement.source.reverse, from - begin, to - from, block->stop, [&](auto bytes, bool reverse) {
        Mix(std::span(sum).subspan(std::size_t(at - start) * channels), bytes, format, at, envelope, reverse);
        at += bytes.size() / (channels * sizeof(std::int16_t));
      });
    });
    sources->audio.FinishBlock();
  }

  entities.each<LivePcmInput>([&](entityx::Entity input, const LivePcmInput& pcm) {
    const auto& frame = pcm.frame;
    if (frame.pts != block->start || frame.sample_count != block->samples || frame.format != format)
      throw std::invalid_argument("Live PCM must match the current audio block");
    Mix(sum, frame.planes.front().AsBytes(), format, start, {0, end, 0, 0, 1.0F, Robot(input)}, false);
  });
  auto samples = std::make_shared<std::vector<std::int16_t>>(sum.size());
  std::ranges::transform(sum, samples->begin(), [](const auto value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
  });
  block->output = {format, {start, format.sample_rate}, block->samples,
                   {BufferReference(std::as_bytes(std::span(*samples)), samples)}};
}

AudioFrame EvaluateAudioBlock(RenderScene& scene, MediaTimestamp start, std::uint32_t samples, AudioFormat format, std::stop_token stop) {
  auto root = scene.Root();
  auto block = root.assign<AudioBlockEvaluation>(start, samples, format);
  struct Release {
    entityx::Entity root;

    ~Release() { root.remove<AudioBlockEvaluation>(); }
  } release{root};
  block->stop = stop;
  scene.RunPhase(SceneSystemPhase::BeforeAudio, stop);
  scene.RunPhase(SceneSystemPhase::Audio, stop);
  return std::move(block->output);
}

} // namespace huxerui::avkit::detail
