#include "timeline_strip.h"
#include "timeline_preview.h"
#include "verification/codec_verification.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "workbench_media.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "support/media_time.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using huxerui::avkit::detail::Add;
using huxerui::avkit::detail::Before;
using huxerui::avkit::detail::ExactTicks;
using huxerui::macos::MetalTexture;
using namespace std::chrono_literals;

constexpr auto kSoftware = CodecPolicy::SoftwareOnly;
constexpr MediaTimestamp Tick(std::int64_t frame) { return {frame, 24}; }
constexpr TimeRange Interval(std::int64_t start, std::int64_t end) { return {Tick(start), Tick(end)}; }

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void CheckStripHandoff(const std::shared_ptr<const StripResult>& current) {
  auto partial = std::make_shared<StripResult>(*current);
  ++partial->generation;
  partial->viewport.range.start = Tick(24);
  const auto ready = current->thumbnails.at(2);
  const auto next_clip = current->thumbnails.at(4);
  partial->thumbnails = {ready};
  partial->thumbnails.front().thumbnail.reset();
  partial->waveform.clear();
  const auto display = UpdateStripSamples(current, partial, false);
  Check(display->thumbnails.size() == 6 && display->thumbnails.front().thumbnail &&
        display->thumbnails.front().thumbnail->image == ready.thumbnail->image &&
        std::ranges::any_of(display->thumbnails, [&](const auto& cell) {
          return cell.clip == next_clip.clip && cell.thumbnail && cell.thumbnail->image == next_clip.thumbnail->image;
        }) && !display->waveform.empty(), "Scrolling cleared usable pictures or the waveform while decoding");
  auto zoomed = std::make_shared<StripResult>(*partial);
  zoomed->viewport.thumbnail_step = Tick(6);
  Check(UpdateStripSamples(display, zoomed, false) == display && UpdateStripSamples(display, zoomed, true) == zoomed,
        "Zoom replaced the displayed strip before the new sampling grid was ready");
  auto relinked = std::make_shared<StripResult>(*partial);
  ++relinked->revision;
  Check(UpdateStripSamples(display, relinked, false) == relinked, "A relink carried stale samples into the new project revision");
}

void CheckInitialProject(const File& directory, const AssetReference& video, const MovieCodecProviders& providers) {
  const File photo_file(directory, "initial-photo.mp4");
  auto photo_image = Bitmap::FromRgba8(97, 55, Bytes(97 * 55 * 4, std::byte{240}));
  const auto photo = PrepareWorkbenchPhoto(std::move(photo_image), photo_file);
  Check(photo.tracks.size() == 1 && photo.tracks.front().kind == TrackKind::Video &&
        photo.tracks.front().presentation == Interval(0, 72), "The photo was not imported as one three-second video clip");
  auto document = CreateWorkbenchProject({video, photo});
  const auto baseline = document.revision;
  Check(baseline->tracks.front().clips.size() == 2 && baseline->assets.front().source == video &&
        baseline->assets.back().source == photo && !document.project->CanUndo() && !document.project->CanRedo(),
        "Initial media order or baseline edit history was incorrect");
  const auto second = baseline->tracks.front().clips.back();
  TimelineSession session(baseline, providers, kSoftware);
  for (const auto at : {Tick(60), Tick(131), Tick(76)}) {
    session.Seek(at);
    const auto frame = session.SampleVideo(at);
    const auto& picture = std::get<VideoFrame>(frame.inputs.front());
    Check(picture.format.visible.width == 98 && picture.format.visible.height == 56, "Seeking lost the initial photo");
  }
  document.project->Edit([&](ProjectEdit& edit) { edit.RemoveClip(second.id); });
  Check(document.project->Undo() && document.project->Snapshot()->tracks == baseline->tracks &&
        !document.project->Undo() && document.project->Redo(), "Undo did not stop at the initial media baseline");
  document.project->Undo();
  const File saved(directory, "initial-media.avkit");
  document.project->Save(saved);
  const auto reopened = OpenWorkbenchProject(saved);
  Check(reopened.revision->tracks == baseline->tracks && !reopened.project->CanUndo(),
        "Reopening lost the initial sequence or exposed its assembly as undo history");
}

Project MakeProject(const AssetReference& asset) {
  Project project(PixelSize{320, 180});
  project.Edit([&](ProjectEdit& edit) {
    const auto source = edit.Import(asset);
    for (const auto& media_track : asset.tracks) {
      const auto track = edit.AddTrack(media_track.kind);
      edit.InsertClip(track, {{source, media_track.id, Interval(6, 54)}, Interval(0, 48)});
      edit.InsertClip(track, {{source, media_track.id, Interval(6, 54), true}, Interval(48, 96)});
    }
  });
  return project;
}

std::vector<std::int16_t> ReferencePcm(const MovieAsset& movie, const MovieCodecProviders& providers) {
  MovieReader reader(movie, providers, TrackKind::Audio, {}, kSoftware);
  std::vector<std::int16_t> pcm;
  const auto channels = movie.pcm.ChannelCount();
  while (auto frame = reader.ReadAudio()) {
    Check(frame->pts == MediaTimestamp{std::int64_t(pcm.size() / channels), movie.pcm.sample_rate}, "Reference audio is discontinuous");
    const auto start = pcm.size(), count = std::size_t(frame->sample_count) * channels;
    pcm.resize(start + count);
    std::memcpy(pcm.data() + start, frame->planes.front().AsBytes().data(), count * sizeof(std::int16_t));
  }
  return pcm;
}

void CheckPeaks(const std::vector<std::int16_t>& pcm, std::uint32_t channels, std::int64_t begin, std::int64_t samples,
                std::span<const WaveformPeak> peaks) {
  Check(peaks.size() == channels && begin >= 0 && std::size_t(begin + samples) * channels <= pcm.size(), "Waveform bucket exceeds reference PCM");
  for (std::size_t channel = 0; channel < channels; ++channel) {
    std::int16_t minimum = 32767, maximum = -32768;
    for (auto at = begin; at < begin + samples; ++at) {
      const auto value = pcm[std::size_t(at) * channels + channel];
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
    Check(std::abs(peaks[channel].minimum - minimum / 32768.0F) < 0.002F &&
          std::abs(peaks[channel].maximum - maximum / 32768.0F) < 0.002F, "Waveform extrema disagree with decoded channel samples");
  }
}

MediaTimestamp FrameAt(MediaTimestamp time) {
  auto pts = time.Rescale(24).value();
  if (Before(time, pts)) --pts.ticks;
  return pts;
}

void CheckStrip(const StripResult& result, const ProjectSnapshot& project, const MovieAsset& movie,
                const std::vector<std::int16_t>& pcm) {
  Check(result.error.empty(), result.error.c_str());
  Check(result.revision == project.revision && !result.thumbnails.empty() && !result.waveform.empty(), "Strip lacks results for the active revision");
  const auto locate = [&](TrackId track, ClipId clip) -> const ClipPlacement& {
    const auto lane = std::ranges::find(project.tracks, track, &TrackInfo::id);
    return std::ranges::find(lane->clips, clip, &ClipInfo::id)->placement;
  };
  std::optional<MediaTimestamp> previous;
  ClipId previous_clip;
  for (const auto& item : result.thumbnails) {
    const auto& clip = locate(item.track, item.clip);
    const auto mapped = MapClipToSource(clip, item.project);
    Check(Before(mapped.start, item.source_time) && Before(item.source_time, mapped.end) && item.thumbnail &&
          item.thumbnail->pts == FrameAt(item.source_time) && item.thumbnail->size == PixelSize{96, 54},
          "Thumbnail did not use the interior source time, actual PTS or pixel size");
    if (clip.source.reverse && previous_clip == item.clip)
      Check(Before(item.thumbnail->pts, *previous), "Reversed thumbnail strip did not descend in source PTS");
    previous = item.thumbnail->pts;
    previous_clip = item.clip;
  }
  const auto rate = movie.pcm.sample_rate, channels = movie.pcm.ChannelCount();
  const auto samples = ExactTicks(result.viewport.bucket_step, rate);
  previous_clip = {};
  for (const auto& bucket : result.waveform) {
    const auto& clip = locate(bucket.track, bucket.clip);
    const auto mapped = MapClipToProject(clip, bucket.source);
    const auto start = Before(mapped.start, result.viewport.range.start) ? result.viewport.range.start : mapped.start;
    const auto end = Before(result.viewport.range.end, mapped.end) ? result.viewport.range.end : mapped.end;
    Check(bucket.project == TimeRange{start, end}, "Waveform bucket did not map from source to project time");
    const auto at = ExactTicks(bucket.source.start, rate);
    CheckPeaks(pcm, channels, at / samples * samples, samples, bucket.peaks);
    if (clip.source.reverse && previous_clip == bucket.clip)
      Check(Before(bucket.source.start, *previous), "Reverse waveform buckets were drawn in source order");
    previous = bucket.source.start;
    previous_clip = bucket.clip;
  }
}

void VerifySourceCaches(const ProjectSnapshot& project, const ProjectSnapshot& relinked, const MovieAsset& movie,
                        const MovieAsset& replacement, const MovieCodecProviders& providers,
                        const std::vector<std::int16_t>& pcm, const std::vector<std::int16_t>& replacement_pcm) {
  verification::DecoderUsage usage;
  const MovieCodecProviders counted{providers.platform, std::make_shared<verification::ObservedCodecProvider>(providers.software, &usage)};
  AssetSampler sampler(counted, MakeStripThumbnail, kSoftware);
  const auto id = project.assets.front().id;
  ThumbnailRequest request{id, movie.video.id, {Tick(12), {1001, 1000}, {1501, 1000}}, {96, 54}};
  const auto pictures = sampler.ReadThumbnails(project, request);
  Check(pictures[0] && pictures[1] && pictures[2] && pictures[0]->pts == Tick(12) && pictures[1]->pts == Tick(24) &&
        pictures[2]->pts == Tick(36) && usage.video.opened == 1 && usage.audio.opened == 0, "Thumbnail requests did not share a video-only decode cursor");
  std::reverse(request.source_times.begin(), request.source_times.end());
  const auto again = sampler.ReadThumbnails(project, request);
  Check(again[0] && again[0]->image == pictures[2]->image && usage.video.opened == 1, "Reverse ordering failed to reuse cached source images");

  const auto fine = sampler.ReadWaveform(project, {id, movie.audio.id, Interval(6, 54), {1, 100}});
  Check(fine.start == Tick(6) && fine.channels == 2 && fine.peaks.size() == 400 && usage.audio.opened == 1,
        "Fine waveform ran outside the stereo trim range");
  const auto merged = sampler.ReadWaveform(project, {id, movie.audio.id, {{7, 25}, {56, 25}}, {1, 50}});
  Check(merged.start == MediaTimestamp{7, 25} && usage.audio.opened == 1 && usage.video.opened == 1,
        "Zooming out failed to merge cached fine buckets without decoding");
  for (std::size_t index = 0; index < merged.peaks.size(); index += merged.channels) {
    const auto start = ExactTicks(merged.start, movie.pcm.sample_rate) + std::int64_t(index / merged.channels) * merged.bucket_interval.ticks;
    CheckPeaks(pcm, merged.channels, start, merged.bucket_interval.ticks, std::span(merged.peaks).subspan(index, merged.channels));
  }

  const auto aligned = sampler.ReadWaveform(project, {id, movie.audio.id, Interval(6, 54), {1, 50}});
  Check(aligned.start == MediaTimestamp{6, 25} && aligned.peaks.size() == 202, "A partially trimmed waveform failed to return aligned full buckets");
  const auto audio_opens = usage.audio.opened;
  const auto changed = sampler.ReadThumbnails(relinked, {id, replacement.video.id, {Tick(12)}, {96, 54}});
  Check(changed.front() && !std::ranges::equal(std::get<Bitmap>(changed.front()->image).Pixels(), std::get<Bitmap>(pictures.front()->image).Pixels()) &&
        usage.video.opened == 2, "Relink reused a thumbnail from the old asset version");
  const auto new_wave = sampler.ReadWaveform(relinked, {id, replacement.audio.id, Interval(6, 54), {1, 50}});
  Check(usage.audio.opened > audio_opens && new_wave.peaks != aligned.peaks, "Relink reused peaks from the old asset version");
  CheckPeaks(replacement_pcm, new_wave.channels, new_wave.start.ticks, new_wave.bucket_interval.ticks,
             std::span(new_wave.peaks).first(new_wave.channels));
  std::cout << "Thumbnail PTS validated with one video-only cursor. Fine stereo peaks merged at the coarser zoom without a new decoder.\n";
  std::cout << "Relink refreshed both source caches at the original asset ID and sample positions.\n";
}

class ThumbnailGate {
public:
  ThumbnailImage Encode(const VideoFrame& frame, PixelSize size) {
    Check(std::this_thread::get_id() != playback_, "Timeline assets are using the playback sequence");
    {
      std::unique_lock lock(mutex_);
      const auto index = ++entered_;
      if (index <= 3 && !changed_.wait_for(lock, 6s, [&] { return released_ >= index; }))
        throw std::runtime_error("Timed out waiting for a newer viewport");
    }
    return MakeStripThumbnail(frame, size);
  }

  unsigned Entered() {
    std::scoped_lock lock(mutex_);
    return entered_;
  }

  void Release(unsigned count) {
    std::scoped_lock lock(mutex_);
    released_ = count;
    changed_.notify_one();
  }

private:
  const std::thread::id playback_ = std::this_thread::get_id();
  std::mutex mutex_;
  std::condition_variable changed_;
  unsigned entered_{}, released_{};
};

} // namespace

void VerifyTimelineStrip(std::string_view first, std::string_view next, std::string_view path) {
  @autoreleasepool {
    const File directory(path), initial_file(directory, "trim-and-reverse.avkit"), relinked_file(directory, "relinked.avkit");
    Check(directory.CreateDirectories() && !initial_file.Exists() && !relinked_file.Exists(), "Verification needs a fresh directory");
    const auto original = ImportMovie(File(first)), replacement = ImportMovie(File(next));
    const auto providers = TimelineProviders();
    const auto original_pcm = ReferencePcm(original, providers), replacement_pcm = ReferencePcm(replacement, providers);
    auto editor = MakeProject(InspectTimelineMovie(original.file));
    editor.Save(initial_file);
    auto project = Project::Open(initial_file);
    const auto base = project.Snapshot();
    Check(base->tracks == editor.Snapshot()->tracks && base->tracks[0].clips[1].placement.source.reverse,
          "Reopening lost the reversed placements");
    Check(MapClipToSource(base->tracks[0].clips[0].placement, Interval(0, 12)) == Interval(6, 18) &&
          MapClipToSource(base->tracks[0].clips[1].placement, Interval(48, 60)) == Interval(42, 54),
          "Clip trims or direction mapped to the wrong source interval");
    const auto new_reference = InspectTimelineMovie(replacement.file);
    auto rebound = Project::Open(initial_file);
    const auto replacement_snapshot = rebound.Edit([&](ProjectEdit& edit) { edit.Relink(base->assets.front().id, new_reference); });
    VerifySourceCaches(*base, *replacement_snapshot, original, replacement, providers, original_pcm, replacement_pcm);
    {
      std::mutex mutex;
      std::condition_variable changed;
      std::shared_ptr<const StripResult> strip;
      std::shared_ptr<const StripResult> first_picture;
      StripWorker worker(providers, MakeStripThumbnail);
      worker.Schedule(base, VisibleStrip(*base, 0, 0), [&](std::shared_ptr<const StripResult> result) {
        std::scoped_lock lock(mutex);
        strip = std::move(result);
        changed.notify_one();
      }, [&](std::shared_ptr<const StripResult> partial) {
        std::scoped_lock lock(mutex);
        if (!first_picture) first_picture = std::move(partial);
      });
      std::unique_lock lock(mutex);
      Check(changed.wait_for(lock, 6s, [&] { return bool(strip); }), "Initial trimmed and reversed strip did not finish");
      CheckStrip(*strip, *base, original, original_pcm);
      Check(first_picture && first_picture->generation == strip->generation && first_picture->waveform.empty() &&
            std::ranges::count_if(first_picture->thumbnails, &StripThumbnail::placeholder_source) == 1 &&
            std::ranges::count_if(first_picture->thumbnails, [](const auto& cell) { return cell.thumbnail.has_value(); }) == 1,
            "The first thumbnail was not published before the strip and waveform completed");
      Check(strip->thumbnails.size() == 8 && strip->thumbnails[0].source_time == Tick(12) &&
            strip->thumbnails[4].source_time == Tick(48), "Unexpected sampling grid across both clips");
      CheckStripHandoff(strip);
    }
    CheckInitialProject(directory, InspectTimelineMovie(original.file), providers);

    TimelineSession playback(base, providers, kSoftware);
    for (const auto [at, expected] : std::array<std::pair<int,int>, 5>{{{0, 6}, {47, 53}, {48, 53}, {49, 52}, {95, 6}}}) {
      const auto frame = playback.SampleVideo(Tick(at));
      Check(frame.inputs.size() == 1 && std::get<VideoFrame>(frame.inputs.front()).pts == Tick(expected),
            "Reverse playback selected the wrong side of the source-frame boundary");
    }
    const auto audio = playback.SampleAudio(Tick(48), 128);
    const auto channels = audio.format.ChannelCount();
    const auto end = ExactTicks(Tick(54), audio.format.sample_rate);
    for (std::size_t sample = 0; sample < audio.sample_count; ++sample) {
      for (std::size_t channel = 0; channel < channels; ++channel) {
        std::int16_t value;
        std::memcpy(&value, audio.planes.front().AsBytes().data() + (sample * channels + channel) * sizeof(value), sizeof(value));
        Check(std::abs(value - original_pcm[std::size_t(end - 1 - sample) * channels + channel]) <= 65,
              "Reverse audio did not preserve channel order and flip sample frames");
      }
    }
    playback.Reset();

    std::mutex deliveries_mutex;
    std::shared_ptr<const StripResult> delivered;
    unsigned deliveries = 0;
    ThumbnailGate gate;
    StripWorker worker(providers, [&](const VideoFrame& frame, PixelSize size) { return gate.Encode(frame, size); });
    std::uint64_t latest{};
    const auto submit = [&](std::shared_ptr<const ProjectSnapshot> snapshot, std::int64_t left, unsigned zoom) {
      const auto viewport = VisibleStrip(*snapshot, left, zoom);
      latest = worker.Schedule(std::move(snapshot), viewport, [&](std::shared_ptr<const StripResult> result) {
        std::scoped_lock lock(deliveries_mutex);
        ++deliveries;
        delivered = std::move(result);
      });
    };
    submit(base, 0, 1);
    std::shared_ptr<const ProjectSnapshot> relinked;
    unsigned action = 0;
    std::optional<MediaTimestamp> blocked_at, relinked_at;
    Bytes original_pixels;
    bool saw_replacement = false;
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    MovieRenderer renderer(base->canvas);
    MetalTexture mailbox({320, 180});
    std::stop_source stop;
    const auto snapshot = [&] {
      MetalTexture::Origin origin;
      return ReadPreviewPixels(mailbox.AcquireFrame(origin));
    };
    const auto stats = PlayTimeline(playback, base->Range(), renderer, mailbox, {}, true,
        [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
          Check(std::chrono::steady_clock::now() < deadline, "Viewport update or playback timed out");
          if (action < 3 && gate.Entered() > action) {
            if (!blocked_at) blocked_at = presented;
            if (!Before(presented, Add(*blocked_at, Tick(2))) && (action < 2 || !Before(presented, Tick(54)))) {
              ++action;
              blocked_at.reset();
              if (action == 1) submit(base, kStripClock / 2, 1);
              if (action == 2) submit(base, 2 * kStripClock, 2);
              if (action == 3) {
                original_pixels = snapshot();
                relinked = project.Edit([&](ProjectEdit& edit) { edit.Relink(base->assets.front().id, new_reference); });
                playback.ApplyProject(relinked);
                submit(relinked, 2 * kStripClock, 2);
                relinked_at = presented;
              }
              gate.Release(action);
              if (relinked_at) return relinked_at;
            }
          }
          if (relinked_at) {
            if (!saw_replacement) {
              Check(snapshot() != original_pixels, "Playback did not switch to relinked pixels on the same timeline");
              saw_replacement = true;
            }
            std::scoped_lock lock(deliveries_mutex);
            if (delivered && !Before(presented, Add(*relinked_at, Tick(3)))) stop.request_stop();
          }
          return std::nullopt;
        }, stop.get_token());
    Check(action == 3 && saw_replacement && stats.clocked_frames > 40 && stats.audio_underruns == 0,
          "Playback did not advance independently through the viewport changes and relink");
    Check(worker.Stats().cancelled == 3 && deliveries == 1 && delivered && delivered->generation == latest,
          "Superseded viewport work delivered results or missed cancellation");
    CheckStrip(*delivered, *relinked, replacement, replacement_pcm);
    const auto archived = project.Snapshot();
    project.Save(relinked_file);
    Check(Project::Open(relinked_file).Snapshot()->assets == archived->assets && archived->assets.front().content_revision > 0,
          "Reopening lost the relinked asset's cache revision");

    std::cout << "Trim and reverse verified against stereo PCM and exact source PTS. " << stats.clocked_frames
              << " clocked preview frames, " << stats.audio_underruns << " audio underruns.\n";
    std::cout << "Scroll, zoom and relink each cancelled active worker work. Only generation " << latest
              << " from project revision " << relinked->revision << " delivered results.\n";
  }
}

} // namespace avkit_preview
