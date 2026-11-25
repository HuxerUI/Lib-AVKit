#include "live_capture.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using Tick = std::int64_t;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

Tick Rescale(MediaTimestamp time, std::uint32_t rate) { return time.Rescale(rate).value().ticks; }

bool HasCaptureCue(const VideoFrame& frame) {
  const auto x = frame.format.visible.x + static_cast<std::uint32_t>(frame.format.visible.width * 0.6F);
  const auto y = frame.format.visible.y + static_cast<std::uint32_t>(frame.format.visible.height * 0.49F);
  const auto sample = [&](int index, std::uint32_t px, std::uint32_t py) {
    const auto& plane = frame.planes[index];
    return std::to_integer<unsigned>(plane.data.AsBytes()[py * plane.row_stride + px * plane.pixel_stride]);
  };
  return sample(0, x, y) > 145 && sample(1, x / 2, y / 2) < 85 && sample(2, x / 2, y / 2) < 80;
}

std::optional<Tick> FindVoiceCue(std::span<const std::int16_t> pcm, std::uint32_t rate) {
  const auto size = rate / 50;
  const auto amplitude = [rate, size](std::span<const std::int16_t> window, double frequency) {
    double real = 0, imaginary = 0;
    const auto step = 2 * std::numbers::pi * frequency / rate;
    for (std::size_t index = 0; index < window.size(); ++index) {
      real += window[index] * std::cos(step * index);
      imaginary += window[index] * std::sin(step * index);
    }
    return 2 * std::hypot(real, imaginary) / size;
  };
  for (std::size_t start = 0; start + size <= pcm.size(); start += size / 2) {
    const auto window = pcm.subspan(start, size);
    if (amplitude(window, 372) > 2300 && amplitude(window, 508) > 2300) return static_cast<Tick>(start);
  }
  return std::nullopt;
}

} // namespace

void VerifyLiveMovie(const File& output, const CameraRecordingStatus& expected, std::optional<std::int64_t> cue_sample) {
  const auto asset = ImportMovie(output);
  const MediaTimestamp duration{expected.samples, expected.sample_rate};
  Check(asset.video_range.start == MediaTimestamp{} && asset.audio_range.start == MediaTimestamp{} &&
        asset.video_range.end == duration && asset.audio_range.end == duration && cue_sample.has_value(),
        "MP4 tracks do not exactly match the capture cutoff");
  Check(asset.pcm.channel_layout == media::AudioChannelLayout::Mono, "The verification cue expects mono PCM");
  MovieReader reader(asset, {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}, CodecPolicy::SoftwareOnly);
  Tick video_end{}, audio_end{};
  std::size_t pictures{};
  std::optional<Tick> video_cue;
  std::vector<std::int16_t> pcm;
  pcm.reserve(expected.samples);
  while (auto frame = reader.ReadVideo()) {
    Check(frame->Validate() && frame->format.pixel_format == VideoPixelFormat::I420 && frame->duration &&
              frame->pts == MediaTimestamp{video_end, expected.sample_rate},
          "Recorded video PTS contains a gap or duplicate");
    if (!video_cue && HasCaptureCue(*frame)) video_cue = video_end;
    video_end += Rescale(*frame->duration, expected.sample_rate);
    ++pictures;
  }
  while (auto frame = reader.ReadAudio()) {
    Check(frame->Validate() && frame->pts == MediaTimestamp{audio_end, expected.sample_rate},
          "Recorded PCM PTS contains a gap or duplicate");
    pcm.resize(audio_end + frame->sample_count);
    std::memcpy(pcm.data() + audio_end, frame->planes.front().AsBytes().data(), frame->sample_count * sizeof(std::int16_t));
    audio_end += frame->sample_count;
  }
  Check(video_end == expected.samples && audio_end == expected.samples && pictures == expected.video_frames,
        "Decoded recording does not end at the common cutoff");
  const auto audio_cue = FindVoiceCue(pcm, expected.sample_rate);
  Check(video_cue && audio_cue && *video_cue >= *cue_sample &&
            std::abs(*audio_cue - *cue_sample) <= expected.sample_rate / 50 &&
            std::abs(*video_cue - *audio_cue) <= expected.sample_rate * 8 / 100,
        "The decoded picture cue and robot voice sidebands do not align");
  std::cout << "AVKit MP4 " << pictures << " frames, " << audio_end << '/' << expected.sample_rate
            << " s, paused " << expected.paused_samples << " samples, cue " << *cue_sample
            << '/' << expected.sample_rate << " s, decoded A/V cue delta "
            << (1000.0 * (*video_cue - *audio_cue) / expected.sample_rate) << " ms " << output.Path() << std::endl;
  std::cout << "The GPU picture cue and processed PCM were verified in the reimported MP4." << std::endl;
}

} // namespace avkit_preview
