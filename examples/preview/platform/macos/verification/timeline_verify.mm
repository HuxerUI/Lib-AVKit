#include "timeline_preview.h"

#include "verification/codec_verification.h"
#include "metal_preview.h"
#include "movie_renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include <huxerui/avkit/edit_template.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
using huxerui::macos::MetalTexture;
using verification::DecoderUsage;
using verification::ObservedCodecProvider;
using verification::VideoFaults;

constexpr PixelSize kCanvas{320, 180};
constexpr huxerui::Rect kPip{0.5625F, 0.0625F, 0.375F, 0.375F};
constexpr huxerui::Rect kMainSticker{0.0625F, 0.5F, 0.375F, 0.375F};
constexpr huxerui::Rect kPipSticker{0.125F, 0.125F, 0.625F, 0.625F};
constexpr huxerui::Rect kEditedSticker{0.3125F, 0.3125F, 0.625F, 0.625F};
constexpr auto kSoftware = CodecPolicy::SoftwareOnly;

constexpr MediaTimestamp Tick(std::int64_t frames) { return {frames, kTimelinePreviewRate}; }
constexpr TimeRange Interval(std::int64_t start, std::int64_t end) { return {Tick(start), Tick(end)}; }

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::uint32_t Track(const AssetReference& asset, TrackKind kind) {
  const auto entry = std::ranges::find(asset.tracks, kind, &AssetTrack::kind);
  Check(entry != asset.tracks.end(), "Timeline fixture lacks a required media track");
  return entry->id;
}

void CheckAssembly(const ProjectSnapshot& a, const ProjectSnapshot& b) {
  Check(a.canvas == b.canvas && a.assets == b.assets && a.tracks == b.tracks && a.transitions == b.transitions && a.Range() == b.Range(),
        "Project assembly or stable identifiers changed");
}

Bytes ReadPreview(MetalTexture& mailbox) {
  MetalTexture::Origin origin;
  const auto picture = mailbox.AcquireFrame(origin);
  Check(origin == MetalTexture::Origin::TopLeft, "Unexpected timeline preview orientation");
  return ReadPreviewPixels(picture);
}

Bytes Sample(TimelineSession& session, MovieRenderer& renderer, MediaTimestamp at, MetalTexture& mailbox) {
  RenderTimelineSample(session, renderer, at, mailbox);
  return ReadPreview(mailbox);
}

std::array<int, 4> PixelAt(const Bytes& picture, std::uint32_t x, std::uint32_t y) {
  const auto offset = (y * kCanvas.width + x) * 4;
  std::array<int, 4> pixel;
  std::ranges::transform(std::span(picture).subspan(offset, 4), pixel.begin(), [](std::byte b) { return std::to_integer<int>(b); });
  return pixel;
}

void CheckFixtureLayers(const Bytes& picture, bool swapped, std::uint32_t pip_sticker_x, std::uint32_t pip_sticker_y) {
  const auto main = PixelAt(picture, 20, 30), pip = PixelAt(picture, 196, 25);
  const auto red = [](const auto& p) { return p[0] > 150 && p[1] < 50 && p[2] < 50 && p[3] == 255; };
  const auto blue = [](const auto& p) { return p[2] > 150 && p[0] < 50 && p[1] < 50 && p[3] == 255; };
  Check(swapped ? red(main) && blue(pip) : blue(main) && red(pip), "Main and picture-in-picture pixels did not match the selected sources");
  Check(PixelAt(picture, 93, 120)[1] > 45 && PixelAt(picture, pip_sticker_x, pip_sticker_y)[1] > 45,
        "The two sticker instances did not blend at their separate positions");
}

void VerifyManual(const File& directory, const AssetReference& first, const AssetReference& second, const File& moved,
                  const MovieCodecProviders& providers, MovieRenderer& renderer) {
  Project project(kCanvas);
  AssetId second_id;
  ClipId pip;
  InstanceId main_sticker, pip_sticker;
  const auto initial = project.Edit([&](ProjectEdit& edit) {
    const auto first_id = edit.Import(first);
    second_id = edit.Import(second);
    const auto main = edit.InsertClip(edit.AddTrack(TrackKind::Video), {{first_id, Track(first, TrackKind::Video), Interval(0, 42)}, Interval(6, 48)});
    pip = edit.InsertClip(edit.AddTrack(TrackKind::Video), {{second_id, Track(second, TrackKind::Video), Interval(6, 30)}, Interval(12, 36)});
    edit.SetLayout(pip, kPip);
    main_sticker = edit.AddSticker(main, {PreviewStickerResource(), kMainSticker});
    pip_sticker = edit.AddSticker(pip, {PreviewStickerResource(), kPipSticker});
    edit.InsertClip(edit.AddTrack(TrackKind::Audio), {{first_id, Track(first, TrackKind::Audio), Interval(0, 42)}, Interval(6, 48)});
  });
  Check(main_sticker != pip_sticker, "Independent sticker instances shared their edit identity");
  MetalTexture mailbox({320, 180});
  const auto at = Tick(18);
  const auto sample = [&](const std::shared_ptr<const ProjectSnapshot>& snapshot, MediaTimestamp time = Tick(18)) {
    TimelineSession session(snapshot, providers, kSoftware);
    return Sample(session, renderer, time, mailbox);
  };
  TimelineSession before(initial, providers, kSoftware);
  const auto two_layers = before.SampleVideo(at);
  const auto& draws = two_layers.plan->draws;
  Check(two_layers.inputs.size() == 3 && draws.size() == 4 && draws[1].input_slot == draws[3].input_slot,
        "Two effects did not share one sticker resource while keeping distinct draws");
  Check(draws[3].destination == huxerui::Rect{0.609375F, 0.109375F, 0.234375F, 0.234375F},
        "The second sticker did not inherit its picture-in-picture transform");
  const auto original = Sample(before, renderer, at, mailbox);
  CheckFixtureLayers(original, false, 241, 38);
  Check(before.SampleVideo(Tick(36)).plan->draws.size() == 2, "A clip rendered at its exclusive end");
  const auto gap = Sample(before, renderer, Tick(0), mailbox);
  const auto silence = before.SampleAudio(Tick(0), 1024);
  Check(std::ranges::all_of(gap, [](std::byte byte) { return byte == std::byte{}; }) &&
        std::ranges::all_of(silence.planes[0].AsBytes(), [](std::byte byte) { return byte == std::byte{}; }),
        "An empty project interval retained a previous picture or audio sample");

  const auto changed = project.Edit([&](ProjectEdit& edit) {
    edit.SetClip(pip, {{second_id, Track(second, TrackKind::Video), Interval(9, 27)}, Interval(15, 33)});
    edit.SetStickerLayout(pip_sticker, kEditedSticker);
  });
  Check(changed->tracks[0] == initial->tracks[0] && changed->tracks[2] == initial->tracks[2] &&
        changed->tracks[1].clips[0].stickers[0] == StickerInstance{pip_sticker, {PreviewStickerResource(), kEditedSticker}},
        "Editing the second instance or its clip affected another track or effect");
  const auto edited = sample(changed);
  CheckFixtureLayers(edited, false, 263, 51);
  Check(edited != original && PixelAt(edited, 93, 120) == PixelAt(original, 93, 120), "Independent sticker edit did not reach the preview pixels");
  bool invalid_edit = false;
  try {
    project.Edit([&](ProjectEdit& edit) {
      edit.SetStickerLayout(pip_sticker, kPipSticker);
      edit.SetClip(pip, {{second_id, Track(second, TrackKind::Audio), Interval(9, 27)}, Interval(15, 33)});
    });
  } catch (const std::invalid_argument&) { invalid_edit = true; }
  Check(invalid_edit && project.Snapshot() == changed, "A failed clip edit published part of its transaction");
  Check(project.Undo(), "Timeline edit was not undoable");
  CheckAssembly(*project.Snapshot(), *initial);
  Check(sample(project.Snapshot()) == original, "Undo did not restore the original pixels");
  Check(project.Redo(), "Timeline edit was not redoable");
  CheckAssembly(*project.Snapshot(), *changed);
  Check(sample(project.Snapshot()) == edited, "Redo did not restore the new pixels");

  const File document(directory, "manual.avkit");
  project.Save(document);
  auto reopened = Project::Open(document);
  CheckAssembly(*project.Snapshot(), *reopened.Snapshot());
  Check(reopened.Snapshot()->revision == project.Snapshot()->revision && sample(reopened.Snapshot()) == edited,
        "Save and reopen changed the revision or preview");
  const auto old_snapshot = reopened.Snapshot();
  before.Reset();
  Check(second.file.MoveTo(moved), "Cannot move the copied picture-in-picture fixture");
  reopened = Project::Open(document);
  Check(reopened.Snapshot()->MissingAssets() == std::vector{second_id}, "Reopening did not report the missing asset identity");
  bool missing_reported = false;
  try { sample(reopened.Snapshot()); }
  catch (const std::runtime_error& error) { missing_reported = std::string_view(error.what()).find(second.file.Path()) != std::string_view::npos; }
  Check(missing_reported, "The unavailable picture did not stop sampling with its source path");
  reopened.Edit([&](ProjectEdit& edit) { edit.Relink(second_id, InspectTimelineMovie(moved)); });
  const auto repaired = reopened.Snapshot();
  Check(repaired->MissingAssets().empty() && repaired->tracks == old_snapshot->tracks && repaired->assets[1].id == second_id &&
        sample(repaired) == edited, "Relink changed the clip, instance identities, or pixels");
  reopened.Save(document);
  reopened = Project::Open(document);
  CheckAssembly(*reopened.Snapshot(), *repaired);
  ExportComposedTimeline(reopened.Snapshot(), File(directory, "manual.mp4"), providers, renderer, kSoftware);
  std::cout << "Manual timeline: independent clip and sticker edits, undo/redo, save/reopen and relink passed\n";
}

Project ApplyRecipe(const AssetReference& first, const AssetReference& second, const EditTemplateRecipe& recipe) {
  Project project(kCanvas);
  std::vector<AssetId> assets;
  project.Edit([&](ProjectEdit& edit) { assets = {edit.Import(first), edit.Import(second)}; });
  const auto empty = project.Snapshot();
  const auto plan = PlanEditTemplate(project, recipe, {assets, {}});
  Check(project.Snapshot() == empty && plan.Prepared().tracks.size() == 3, "Planning modified the source project or missed a track");
  const auto remapped = PlanEditTemplate(project, recipe, {assets, {{"lead", assets[1]}}});
  Check(remapped.Prepared().tracks[0].clips[0].placement.source.asset == assets[1] &&
        remapped.Prepared().tracks[1].clips[0].placement.source.asset == assets[0],
        "Explicit slot binding did not take precedence over selection order");
  const auto applied = ApplyEditTemplate(project, plan);
  CheckAssembly(*applied, plan.Prepared());
  Check(applied->revision == empty->revision + 1 && project.Undo() && project.Snapshot()->tracks.empty() && project.Redo(),
        "Template assembly did not behave as one transaction");
  CheckAssembly(*project.Snapshot(), *applied);
  const auto current = project.Snapshot();
  bool stale = false;
  try { ApplyEditTemplate(project, plan); }
  catch (const std::invalid_argument&) { stale = true; }
  Check(stale && project.Snapshot() == current, "A stale plan changed the edited project");
  return project;
}

TimelinePlaybackStats VerifyPlayback(const ProjectSnapshot& project, TimelineSession& session,
                                     MovieRenderer& renderer, const Bytes& first, const Bytes& second) {
  MetalTexture mailbox({320, 180});
  std::stop_source stop;
  int seek_stage = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  const auto stats = PlayTimeline(session, project.Range(), renderer, mailbox, Tick(6), true,
      [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
        Check(std::chrono::steady_clock::now() < deadline, "Clocked preview did not complete the seek sequence in time");
        if (seek_stage == 0 && presented.Compare(Tick(16)) != -1) {
          CheckFixtureLayers(ReadPreview(mailbox), false, 241, 38);
          ++seek_stage;
          return Tick(30);
        }
        if (seek_stage == 1 && presented == Tick(30)) {
          Check(ReadPreview(mailbox) == second, "Clocked playback published the wrong frame after a forward seek");
          ++seek_stage;
        }
        if (seek_stage == 2 && presented.Compare(Tick(33)) != -1) {
          ++seek_stage;
          return Tick(6);
        }
        if (seek_stage == 3 && presented == Tick(6)) {
          Check(ReadPreview(mailbox) == first, "Clocked playback published the wrong frame after a backward seek");
          ++seek_stage;
        }
        if (seek_stage == 4 && presented.Compare(Tick(9)) != -1) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(seek_stage == 4 && stats.clocked_frames > 0, "Clocked preview did not resume after both seeks");
  return stats;
}

void VerifyExports(const File& first, const File& second, const MovieCodecProviders& providers,
                   MovieRenderer& renderer, const Bytes& early_preview, const Bytes& late_preview,
                   bool identical_video = true) {
  const auto a_asset = ImportMovie(first), b_asset = ImportMovie(second);
  const auto two_seconds = Tick(48);
  const auto complete = [&](const MovieAsset& asset) {
    return asset.video_range.start == MediaTimestamp{} && asset.audio_range.start == MediaTimestamp{} &&
           asset.video_range.end == two_seconds && asset.audio_range.end == two_seconds;
  };
  Check(complete(a_asset) && complete(b_asset) && a_asset.pcm == b_asset.pcm, "Export changed the audio or video presentation range");
  MovieReader a(a_asset, providers, kSoftware), b(b_asset, providers, kSoftware);
  MetalTexture mailbox({320, 180});
  const auto plan = std::make_shared<RenderPlan>(RenderPlan{{kCanvas, render::ImageFormat::Bgra8Unorm}, {{0, Blend::Replace}}});
  const auto check_preview = [&](const VideoFrame& frame, const Bytes& preview) {
    PublishPreviewSurface(mailbox, renderer.Draw({frame.pts, plan, {frame}}));
    const auto decoded = ReadPreview(mailbox);
    for (const auto [x, y] : std::array<std::pair<unsigned, unsigned>, 4>{{{20, 30}, {196, 25}, {93, 120}, {241, 38}}}) {
      const auto actual = PixelAt(decoded, x, y), expected = PixelAt(preview, x, y);
      for (int channel = 0; channel < 3; ++channel)
        Check(std::abs(actual[channel] - expected[channel]) <= 16, "Reimported export exceeded preview color tolerance");
    }
  };
  for (std::int64_t index = 0; index < 48; ++index) {
    const auto left = a.ReadVideo(), right = b.ReadVideo();
    Check(left && right && left->Validate() && right->Validate() && left->format == right->format &&
          left->pts == Tick(index) && left->pts == right->pts && left->duration == Tick(1) && left->duration == right->duration &&
          left->planes.size() == right->planes.size(), "Exported video PTS, duration or format diverged");
    if (identical_video) {
      for (std::size_t plane = 0; plane < left->planes.size(); ++plane) {
        const auto& lhs = left->planes[plane];
        const auto& rhs = right->planes[plane];
        Check(lhs.row_stride == rhs.row_stride && lhs.pixel_stride == rhs.pixel_stride &&
              std::ranges::equal(lhs.data.AsBytes(), rhs.data.AsBytes()), "Decoded export pixels differed between template applications");
      }
    }
    if (index == 6 || index == 23 || index == 24 || index == 30) {
      const auto& preview = index < 24 ? early_preview : late_preview;
      check_preview(*left, preview);
      if (!identical_video) check_preview(*right, preview);
    }
  }
  Check(!a.ReadVideo() && !b.ReadVideo(), "Exported video exceeded the project range");
  const auto rate = a_asset.pcm.sample_rate;
  std::int64_t samples = 0;
  while (const auto left = a.ReadAudio()) {
    const auto right = b.ReadAudio();
    Check(right && left->Validate() && right->Validate() && left->pts == MediaTimestamp{samples, rate} && left->pts == right->pts &&
          left->sample_count == right->sample_count && std::ranges::equal(left->planes[0].AsBytes(), right->planes[0].AsBytes()),
          "Decoded export PCM or its PTS diverged");
    samples += left->sample_count;
  }
  Check(!b.ReadAudio() && samples == 2 * rate, "Exported audio and video did not end together");
  std::cout << "Reimport: 48 video frames at 24 fps, " << samples << " identical PCM samples with exact PTS"
            << (identical_video ? " and identical decoded video\n" : " and verified pixels at both sides of the cut\n");
}

MovieCodecProviders Observe(const MovieCodecProviders& providers, DecoderUsage& usage, bool inject_fault) {
  const VideoFaults faults{.decode_after = inject_fault ? 17u : 0u};
  return {std::make_shared<ObservedCodecProvider>(providers.platform, &usage, faults),
          std::make_shared<ObservedCodecProvider>(providers.software, &usage)};
}

void CheckDecoderUsage(const DecoderUsage& usage, std::size_t video_opens, std::size_t audio_opens) {
  std::cout << "Decoder opens: " << usage.video.opened << " video, " << usage.audio.opened << " audio. Concurrent peak: "
            << usage.peak << " (" << usage.video.peak << " video, " << usage.audio.peak << " audio).\n";
  Check(usage.video.opened == video_opens && usage.audio.opened == audio_opens && usage.video.peak == 2 &&
        usage.audio.peak == 1 && usage.peak == 3 && !usage.video.active && !usage.audio.active,
        "Timeline opened extra decoders, exceeded three active sessions or retained them after shutdown");
}

void CheckDecodeFallback(const std::vector<CodecFallback>& events, bool injected) {
  Check(events.size() == static_cast<std::size_t>(injected), "Unexpected number of codec fallbacks");
  if (!injected) return;
  const auto& event = events.front();
  Check(event.operation == CodecOperation::VideoDecode && event.cause.code == CodecErrorCode::ResourceExhausted &&
        event.from == "Lib-Media" && event.to == "FFmpeg" && event.resume_at && event.resume_at->Compare({}) == 1,
        "The active VideoToolbox decode failure did not resume in FFmpeg at the last picture's source PTS");
  std::cout << "VideoToolbox decode recovered in FFmpeg at " << event.resume_at->ticks << '/' << event.resume_at->timescale << '\n';
}

constexpr auto kWipeStart = 15;
constexpr auto kWipeEnd = 45;
constexpr auto kWipeLength = kWipeEnd - kWipeStart;
constexpr auto kTransitionMovieFrames = 60;
constexpr huxerui::Rect kTransitionRect{0.2F, 0.2F, 0.6F, 0.6F};
constexpr huxerui::Rect kOutgoingSticker{0.3671875F, 0.78125F, 0.625F, 0.5F};
constexpr huxerui::Rect kIncomingSticker{-0.1328125F, -0.21875F, 0.625F, 0.5F};
constexpr std::pair<unsigned, unsigned> kOutgoingOverflow{208, 151};
constexpr std::pair<unsigned, unsigned> kIncomingOverflow{112, 29};

Project MakeTransitionProject(const AssetReference& first, const AssetReference& second, const MovieCodecProviders& providers) {
  Project project(kCanvas);
  InstanceId instance, outgoing_sticker, incoming_sticker;
  const std::vector<Keyframe<float>> keys{{Tick(0), 0.2F}, {Tick(6), 0.52F, Interpolation::Hold},
                                          {Tick(12), 0.8F}, {Tick(24), 0.28F}};
  const auto original = project.Edit([&](ProjectEdit& edit) {
    const auto first_id = edit.Import(first), second_id = edit.Import(second);
    const auto video = edit.AddTrack(TrackKind::Video);
    const auto from = edit.InsertClip(video, {{first_id, Track(first, TrackKind::Video), Interval(0, 48)}, Interval(0, 48)});
    const auto to = edit.InsertClip(video, {{second_id, Track(second, TrackKind::Video), Interval(6, 54)}, Interval(12, 60)});
    edit.SetLayout(from, kTransitionRect);
    edit.SetLayout(to, kTransitionRect);
    outgoing_sticker = edit.AddSticker(from, {PreviewStickerResource(), kOutgoingSticker});
    incoming_sticker = edit.AddSticker(to, {PreviewStickerResource(), kIncomingSticker});
    const auto audio = edit.AddTrack(TrackKind::Audio);
    edit.InsertClip(audio, {{first_id, Track(first, TrackKind::Audio), Interval(0, 60)}, Interval(0, 60)});
    instance = edit.SetTransition(from, to, {Interval(18, 42), PreviewWipeResource()});
    edit.SetTransitionFeather(instance, 0.37F, keys);
  });
  Check(outgoing_sticker != incoming_sticker, "Two clips must have independent instances of the shared sticker");
  const auto moved = project.Edit([&](ProjectEdit& edit) { edit.SetTransitionRange(instance, Interval(kWipeStart, 39)); });
  Check(moved->transitions.size() == 1 && moved->transitions.front().id == instance &&
        moved->transitions.front().spec.feather_keys == keys && moved->tracks == original->tracks,
        "Moving the transition changed its identity, local keys or clip placements");
  {
    TimelineSession before(original, providers, kSoftware), after(moved, providers, kSoftware);
    const auto first_sample = before.SampleVideo(Tick(27));
    const auto moved_sample = after.SampleVideo(Tick(24));
    Check(first_sample.masks.size() == 1 && first_sample.masks == moved_sample.masks &&
          first_sample.plan->draws == moved_sample.plan->draws && first_sample.plan->subcompositions == moved_sample.plan->subcompositions,
          "The same local time sampled differently after moving the transition");
  }

  const auto resized = project.Edit([&](ProjectEdit& edit) { edit.SetTransitionRange(instance, Interval(kWipeStart, kWipeEnd)); });
  auto expected = keys;
  expected[1].time = {15, 48};
  expected[2].time = Tick(15);
  expected[3].time = Tick(kWipeLength);
  const auto& transition = resized->transitions.front();
  Check(transition.id == instance && transition.spec.range == Interval(kWipeStart, kWipeEnd) &&
        transition.spec.mask == PreviewWipeResource() && transition.spec.feather_keys == expected && resized->tracks == moved->tracks,
        "Resizing did not preserve the transition and retime all keys exactly");
  bool rejected = false;
  try { project.Edit([&](ProjectEdit& edit) { edit.SetTransitionRange(instance, Interval(kWipeStart, 49)); }); }
  catch (const std::invalid_argument&) { rejected = true; }
  Check(rejected && project.Snapshot() == resized, "A transition outside the two source clips changed the project");
  Check(project.Undo() && project.Snapshot()->transitions == moved->transitions && project.Redo() &&
        project.Snapshot()->transitions == resized->transitions, "Undo or redo did not restore the transition range and keys");
  std::cout << "Transition instance " << instance.value << " retained through move and resize. Local keys:"
            << " 0, 6/24, 12/24, 24/24 -> 0, 15/48, 15/24, 30/24 seconds\n";
  std::cout << "Sticker instances " << outgoing_sticker.value << " and " << incoming_sticker.value
            << " retained their respective clips and layouts\n";
  return project;
}

float ExpectedFeather(int offset) {
  if (offset < 7.5F) return std::lerp(0.2F, 0.52F, offset / 7.5F);
  if (offset < 15) return 0.52F;
  return std::lerp(0.8F, 0.28F, (offset - 15) / 15.0F);
}

void CheckTransitionFrame(const TimelineFrame& frame, int index) {
  Check(frame.pts == Tick(index), "Transition sample used the wrong project PTS");
  const auto& plan = *frame.plan;
  const auto input_pts = [&](std::uint32_t slot) { return std::get<VideoFrame>(frame.inputs.at(slot)).pts; };
  const auto check_sticker = [&](const Draw& draw, huxerui::Rect destination) {
    const auto* resource = std::get_if<ResourceRef>(&frame.inputs.at(draw.input_slot));
    const huxerui::Rect canvas_destination{kTransitionRect.x + kTransitionRect.width * destination.x,
                                           kTransitionRect.y + kTransitionRect.height * destination.y,
                                           kTransitionRect.width * destination.width, kTransitionRect.height * destination.height};
    Check(resource && *resource == PreviewStickerResource() && draw.destination == canvas_destination &&
          draw.blend == Blend::PremultipliedOver && !draw.masked_blend, "A sticker escaped its clip or changed resources");
  };
  if (index < kWipeStart || index >= kWipeEnd) {
    const bool outgoing = index < kWipeStart;
    Check(plan.draws.size() == 2 && plan.subcompositions.empty() && !plan.draws[0].masked_blend && plan.draws[0].destination == kTransitionRect &&
          frame.masks.empty() && frame.inputs.size() == 2 && input_pts(plan.draws[0].input_slot) == Tick(outgoing ? index : index - 6),
          "A source outside the half-open transition sampled an inactive clip or mask");
    check_sticker(plan.draws[1], outgoing ? kOutgoingSticker : kIncomingSticker);
    return;
  }
  Check(plan.draws.size() == 1 && plan.subcompositions.size() == 2 && frame.masks.size() == 1 && frame.inputs.size() == 3,
        "An active transition must draw only the blend of two clip subcompositions");
  const auto& draw = plan.draws.front();
  const huxerui::Rect full{0, 0, 1, 1};
  Check(draw.masked_blend && draw.input_slot == frame.inputs.size() && draw.masked_blend->incoming_slot == frame.inputs.size() + 1 &&
        draw.destination == full && draw.blend == Blend::PremultipliedOver && draw.masked_blend->mask_region == kTransitionRect,
        "The clip-local mask must combine two canvas textures and preserve lower layers");
  const auto& outgoing = plan.subcompositions[0].draws;
  const auto& incoming = plan.subcompositions[1].draws;
  Check(outgoing.size() == 2 && incoming.size() == 2 && outgoing[0].destination == kTransitionRect && incoming[0].destination == kTransitionRect &&
        outgoing[0].blend == Blend::Replace && incoming[0].blend == Blend::Replace && outgoing[1].input_slot == incoming[1].input_slot,
        "Each clip pass must draw its own picture and layout with a shared sticker texture");
  check_sticker(outgoing[1], kOutgoingSticker);
  check_sticker(incoming[1], kIncomingSticker);
  const auto& blend = *draw.masked_blend;
  const auto& sample = frame.masks.at(blend.mask_slot);
  const auto offset = index - kWipeStart;
  Check(input_pts(outgoing[0].input_slot) == Tick(index) && input_pts(incoming[0].input_slot) == Tick(index - 6) &&
        sample.resource == PreviewWipeResource() && sample.offset == Tick(offset) && sample.duration == Tick(kWipeLength) &&
        sample.FrameIndex(kPreviewWipeFrameCount) == offset * kPreviewWipeFrameCount / kWipeLength &&
        std::abs(blend.feather - ExpectedFeather(offset)) < 0.00001F && !blend.inverted,
        "Transition source time, mask frame or interpolated feather diverged");
}

void CheckCoveragePixels(const std::vector<Bytes>& pictures) {
  const auto& from = pictures.front();
  const auto& to = pictures.back();
  Check(from != to && pictures[kWipeStart - 1] == pictures[kWipeStart] && pictures[kWipeStart] == from &&
        pictures[kWipeEnd - 1] == pictures[kWipeEnd] && pictures[kWipeEnd] == to,
        "Entering or leaving the transition changed the sticker or its overflow pixels");
  const auto& middle = pictures[kWipeStart + kWipeLength / 2];
  const auto [out_x, out_y] = kOutgoingOverflow;
  const auto [in_x, in_y] = kIncomingOverflow;
  const auto outgoing = PixelAt(from, out_x, out_y), incoming = PixelAt(to, in_x, in_y);
  Check(outgoing[3] > 0 && outgoing[3] < 255 && PixelAt(to, out_x, out_y)[3] == 0 &&
        incoming[3] > 0 && incoming[3] < 255 && PixelAt(from, in_x, in_y)[3] == 0 &&
        PixelAt(middle, out_x, out_y) == outgoing && PixelAt(middle, in_x, in_y) == incoming,
        "Each sticker's overflow must be visible and follow its own side of the mask");
  const auto partially_masked = PixelAt(pictures[kWipeStart + 19], out_x, out_y)[3];
  Check(partially_masked > 0 && partially_masked < outgoing[3], "The mask did not partially fade the overflow within the transition");
  for (int index = kWipeStart; index < kWipeEnd; ++index) {
    const int offset = index - kWipeStart;
    const int mask = offset * kPreviewWipeFrameCount / kWipeLength;
    if (mask == 0 || mask == kPreviewWipeFrameCount - 1) continue;
    const auto feather = ExpectedFeather(offset);
    for (const int y : {17, 29, 43, 85, 137, 151, 167}) {
      for (int x = 17; x < 320; x += 11) {
        const auto column = std::clamp((x - 64) / 3, 0, 63);
        const auto row = std::clamp((y - 36) / 3, 0, 35);
        const auto coverage = std::clamp(128 + 12 * (8 * (mask + 1) - column) + 2 * (18 - row), 0, 255) / 255.0F;
        const auto t = std::clamp((coverage - (0.5F - feather / 2)) / feather, 0.0F, 1.0F);
        const auto weight = t * t * (3.0F - 2.0F * t);
        const auto a = PixelAt(from, x, y), b = PixelAt(to, x, y), actual = PixelAt(pictures[index], x, y);
        for (int channel = 0; channel < 4; ++channel) {
          const auto expected = std::lerp(float(a[channel]), float(b[channel]), weight);
          Check(std::abs(actual[channel] - expected) <= 2.0F, "GPU mask coverage or feather exceeded pixel tolerance");
        }
      }
    }
  }
}

void VerifyTransitionOverLowerLayer(TimelineSession& session, MovieRenderer& renderer, MetalTexture& mailbox,
                                    const std::vector<Bytes>& pictures) {
  auto frame = session.SampleVideo(Tick(kWipeStart + kWipeLength / 2));
  auto plan = std::make_shared<RenderPlan>(*frame.plan);
  const auto lower = plan->subcompositions.front().draws.front().input_slot;
  plan->draws.insert(plan->draws.begin(), {lower, Blend::Replace});
  frame.plan = std::move(plan);
  PublishPreviewSurface(mailbox, renderer.Draw(frame));
  const auto pixels = ReadPreview(mailbox);
  Check(PixelAt(pixels, 20, 20) == PixelAt(pictures.front(), 160, 90), "The transition cleared pixels from a lower layer");
}

void VerifyTransitionSeeks(const ProjectSnapshot& project, TimelineSession& session, MovieRenderer& renderer,
                           const std::vector<Bytes>& pictures) {
  MetalTexture mailbox({320, 180});
  for (const auto index : {14, 15, 30, 45, 44, 22, 37}) {
    auto frame = session.SampleVideo(Tick(index));
    CheckTransitionFrame(frame, index);
    PublishPreviewSurface(mailbox, renderer.Draw(frame));
    Check(ReadPreview(mailbox) == pictures[index], "An explicit timeline seek changed the transition pixels");
  }
  session.Reset();
  std::stop_source stop;
  int stage = 0;
  std::optional<MediaTimestamp> last;
  bool saw_before = false, saw_after = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  const auto stats = PlayTimeline(session, project.Range(), renderer, mailbox, Tick(10), true,
      [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
        Check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for both clocked transition seeks");
        if (last != presented) {
          const auto index = presented.Rescale(kTimelinePreviewRate)->ticks;
          Check(ReadPreview(mailbox) == pictures.at(index), "Clocked preview changed the transition pixels");
          saw_before = saw_before || index < kWipeStart;
          saw_after = saw_after || index >= kWipeEnd;
          last = presented;
        }
        if (stage == 0 && presented.Compare(Tick(21)) != -1) { ++stage; return Tick(40); }
        if (stage == 1 && presented == Tick(40)) ++stage;
        if (stage == 2 && presented.Compare(Tick(47)) != -1) { ++stage; return Tick(23); }
        if (stage == 3 && presented == Tick(23)) ++stage;
        if (stage == 4 && presented.Compare(Tick(27)) != -1) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(stage == 4 && saw_before && saw_after && stats.clocked_frames > 0 && stats.audio_underruns == 0,
        "Clocked playback did not cross both transition boundaries and resume after both seeks");
}

void VerifyTransitionExport(const File& output, const std::shared_ptr<const ProjectSnapshot>& project,
                            const MovieCodecProviders& providers, MovieRenderer& renderer, const std::vector<Bytes>& pictures) {
  FileExportSettings settings;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  settings.policy = kSoftware;
  int written = 0;
  ExportTimeline(project, output, providers, settings, [&](const TimelineFrame& frame, const VideoEncoderConfig& config,
                                                         MediaTimestamp duration) {
    CheckTransitionFrame(frame, written);
    auto surface = renderer.Draw(frame);
    Check(ReadPreviewPixels(huxerui::avkit::macos::GetMetalTexture(surface)) == pictures.at(written),
          "Export rendered different transition pixels from preview at the same PTS");
    ++written;
    return renderer.EncoderFrame(surface, config, frame.pts, duration);
  });
  Check(written == kTransitionMovieFrames, "Export sampled the wrong number of pictures");
  const auto movie = ImportMovie(output);
  MovieReader reader(movie, providers, kSoftware);
  const auto plan = std::make_shared<RenderPlan>(RenderPlan{{kCanvas, render::ImageFormat::Bgra8Unorm}, {{0, Blend::Replace}}});
  double worst_mean = 0;
  for (int index = 0; index < written; ++index) {
    const auto frame = reader.ReadVideo();
    Check(frame && frame->Validate() && frame->pts == Tick(index) && frame->duration == Tick(1), "Reimport changed video timing");
    const auto surface = renderer.Draw({frame->pts, plan, {*frame}});
    const auto pixels = ReadPreviewPixels(huxerui::avkit::macos::GetMetalTexture(surface));
    const auto& expected = pictures[index];
    std::uint64_t difference = 0;
    for (std::size_t i = 0; i < expected.size(); i += 4) {
      for (std::size_t channel = 0; channel < 3; ++channel)
        difference += std::abs(std::to_integer<int>(pixels[i + channel]) - std::to_integer<int>(expected[i + channel]));
    }
    worst_mean = std::max(worst_mean, double(difference) / (expected.size() * 0.75));
    for (const auto [x, y] : {kOutgoingOverflow, kIncomingOverflow}) {
      const auto before = PixelAt(expected, x, y), after = PixelAt(pixels, x, y);
      const auto delta = std::abs(before[0] - after[0]) + std::abs(before[1] - after[1]) + std::abs(before[2] - after[2]);
      Check(delta <= 36, "Reimport altered RGB at a sticker overflow pixel");
    }
  }
  Check(!reader.ReadVideo() && worst_mean <= 6.0, "Reimported transition exceeded the color tolerance");
  std::int64_t samples = 0;
  while (const auto audio = reader.ReadAudio()) {
    Check(audio->Validate() && audio->pts == MediaTimestamp{samples, movie.pcm.sample_rate}, "Reimport changed the PCM timeline");
    samples += audio->sample_count;
  }
  Check(MediaTimestamp{samples, movie.pcm.sample_rate} == Tick(kTransitionMovieFrames), "Exported audio and video did not end together");
  std::cout << "Export and preview matched before encoding at all " << written << " frames. Reimport: " << samples
            << " PCM samples, exact PTS, worst frame mean RGB error " << worst_mean << " / 255\n";
}

} // namespace

void VerifyTimelinePreview(std::string_view first, std::string_view second, std::string_view path) {
  @autoreleasepool {
    const File directory(path), lead(directory, "main source.mp4"), overlay(directory, "pip source.mp4"), moved(directory, "relinked PiP source.mp4");
    Check(directory.CreateDirectories() && File(first).CopyTo(lead) && File(second).CopyTo(overlay),
          "Verification requires both fixtures and a directory without existing test outputs");
    const auto providers = TimelineProviders();
    MovieRenderer renderer(kCanvas);
    const auto lead_asset = InspectTimelineMovie(lead);
    VerifyManual(directory, lead_asset, InspectTimelineMovie(overlay), moved, providers, renderer);

    const StickerSpec main_sticker{PreviewStickerResource(), kMainSticker}, pip_sticker{PreviewStickerResource(), kPipSticker};
    const EditTemplateRecipe recipe{{"avkit.preview", "1", "two-shot"}, kCanvas, {"lead", "overlay"}, {
      {TrackKind::Video, {{"lead", Interval(6, 30), Interval(0, 24), {0, 0, 1, 1}, {main_sticker}},
                         {"overlay", Interval(12, 36), Interval(24, 48), {0, 0, 1, 1}, {main_sticker}}}},
      {TrackKind::Video, {{"overlay", Interval(0, 24), Interval(0, 24), kPip, {pip_sticker}},
                         {"lead", Interval(6, 30), Interval(24, 48), kPip, {pip_sticker}}}},
      {TrackKind::Audio, {{"lead", Interval(6, 30), Interval(0, 24)}, {"overlay", Interval(12, 36), Interval(24, 48)}}},
    }};
    const auto moved_asset = InspectTimelineMovie(moved);
    const auto first_project = ApplyRecipe(lead_asset, moved_asset, recipe);
    const auto second_project = ApplyRecipe(lead_asset, moved_asset, recipe);
    const File first_doc(directory, "template-a.avkit"), second_doc(directory, "template-b.avkit");
    first_project.Save(first_doc);
    second_project.Save(second_doc);
    const auto a_project = Project::Open(first_doc).Snapshot(), b_project = Project::Open(second_doc).Snapshot();
    CheckAssembly(*a_project, *b_project);
    TimelineSession a(a_project, providers, kSoftware), b(b_project, providers, kSoftware);
    MetalTexture mailbox({320, 180});
    Bytes early, late;
    for (const auto frame : {6, 18, 24, 30, 6}) {
      const auto render_sample = [&](TimelineSession& session) {
        const auto sample = session.SampleVideo(Tick(frame));
        Check(sample.pts == Tick(frame) && std::get<VideoFrame>(sample.inputs.at(0)).pts == Tick(frame + (frame < 24 ? 6 : -12)) &&
              std::get<VideoFrame>(sample.inputs.at(2)).pts == Tick(frame + (frame < 24 ? 0 : -18)),
              "Template preview decoded a different source PTS from the selected trim");
        PublishPreviewSurface(mailbox, renderer.Draw(sample));
        return ReadPreview(mailbox);
      };
      const auto pixels = render_sample(a);
      Check(pixels == render_sample(b), "Equivalent template projects rendered different preview pixels");
      CheckFixtureLayers(pixels, frame >= 24, 241, 38);
      if (frame == 6) {
        Check(early.empty() || early == pixels, "Backward seek retained pixels from the later template shot");
        early = pixels;
      }
      if (frame == 30) late = pixels;
    }
    a.Reset();
    b.Reset();
    VerifyPlayback(*a_project, a, renderer, early, late);
    VerifyPlayback(*b_project, b, renderer, early, late);
    a.Reset();
    b.Reset();
    std::cout << "Template: matching assemblies, source PTS, preview pixels and both audio-clock seek directions\n";

    const File first_output(directory, "template-a.mp4"), second_output(directory, "template-b.mp4");
    ExportComposedTimeline(a_project, first_output, providers, renderer, kSoftware);
    ExportComposedTimeline(b_project, second_output, providers, renderer, kSoftware);
    VerifyExports(first_output, second_output, providers, renderer, early, late);
  }
}

void VerifyTimelineCodecs(std::string_view path, std::string_view directory_path) {
  @autoreleasepool {
    const auto project = Project::Open(File(path)).Snapshot();
    const File directory(directory_path), baseline(directory, "platform.mp4"), fallback(directory, "fallback.mp4");
    Check(project->canvas == kCanvas && project->Range() == Interval(0, 48) && project->tracks.size() == 3,
          "Codec verification requires the two-shot template project from --timeline-verify");
    Check(directory.CreateDirectories() && !baseline.Exists() && !fallback.Exists(), "Codec verification needs new MP4 paths");
    const auto providers = TimelineProviders();
    MovieRenderer renderer(kCanvas);
    Bytes early, late;
    {
      MetalTexture mailbox({320, 180});
      TimelineSession session(project, providers);
      early = Sample(session, renderer, Tick(6), mailbox);
      late = Sample(session, renderer, Tick(30), mailbox);
    }

    for (const bool inject_fault : {false, true}) {
      DecoderUsage usage;
      std::vector<CodecFallback> events;
      const auto label = inject_fault ? " (injected video decode failure)" : " (platform)";
      const auto report = [&](const CodecFallback& event) { events.push_back(event); };
      TimelinePlaybackStats stats;
      {
        TimelineSession session(project, Observe(providers, usage, inject_fault), CodecPolicy::Auto,
            [&](const CodecFallback& event) {
              Check(usage.video.active == 2 && usage.audio.active == 1, "Preview fallback interrupted an unrelated decoder");
              report(event);
            });
        stats = VerifyPlayback(*project, session, renderer, early, late);
      }
      std::cout << "Preview and seeks" << label << '\n';
      CheckDecoderUsage(usage, 6 + static_cast<std::size_t>(inject_fault), 3);
      CheckDecodeFallback(events, inject_fault);
      Check(stats.audio_underruns == 0 && stats.maximum_video_lag_ms < 100, "Preview lost audio-clock synchronization");
      std::cout << "Clocked frames: " << stats.clocked_frames << ", maximum video lag " << stats.maximum_video_lag_ms
                << " ms, audio underruns " << stats.audio_underruns << '\n';

      usage = {};
      events.clear();
      ExportComposedTimeline(project, inject_fault ? fallback : baseline, Observe(providers, usage, inject_fault),
                             renderer, CodecPolicy::Auto, report);
      std::cout << "Export" << label << '\n';
      CheckDecoderUsage(usage, 4 + static_cast<std::size_t>(inject_fault), 2);
      CheckDecodeFallback(events, inject_fault);
    }
    VerifyExports(baseline, fallback, providers, renderer, early, late, false);
  }
}

void VerifyTimelineTransition(std::string_view first, std::string_view second, std::string_view path) {
  @autoreleasepool {
    const File directory(path), document(directory, "animated-transition.avkit"), output(directory, "animated-transition.mp4");
    Check(directory.CreateDirectories() && !document.Exists() && !output.Exists(), "Transition verification needs a fresh directory");
    const auto providers = TimelineProviders();
    const auto edited = MakeTransitionProject(InspectTimelineMovie(File(first)), InspectTimelineMovie(File(second)), providers);
    edited.Save(document);
    const auto project = Project::Open(document).Snapshot();
    CheckAssembly(*project, *edited.Snapshot());

    MovieRenderer renderer(kCanvas);
    MetalTexture mailbox({320, 180});
    TimelineSession session(project, providers, kSoftware);
    std::vector<Bytes> pictures;
    for (int index = 0; index < kTransitionMovieFrames; ++index) {
      const auto frame = session.SampleVideo(Tick(index));
      CheckTransitionFrame(frame, index);
      PublishPreviewSurface(mailbox, renderer.Draw(frame));
      pictures.push_back(ReadPreview(mailbox));
    }
    CheckCoveragePixels(pictures);
    VerifyTransitionOverLowerLayer(session, renderer, mailbox, pictures);
    const auto fractional_key = session.SampleVideo({45, 48});
    Check(fractional_key.masks.at(0).offset == MediaTimestamp{15, 48} &&
          fractional_key.masks.at(0).FrameIndex(kPreviewWipeFrameCount) == 2 &&
          fractional_key.plan->draws.front().masked_blend.value().feather == 0.52F,
          "The retimed half-frame key did not sample exactly at the mask-frame boundary");
    session.Reset();
    VerifyTransitionSeeks(*project, session, renderer, pictures);
    session.Reset();
    std::cout << "Sticker overflow survived both transition boundaries, followed the animated mask, and preserved lower layers\n";
    std::cout << "Preview and both seek directions matched all eight mask frames, the retimed feather curve and both clip-local stickers\n";
    VerifyTransitionExport(output, project, providers, renderer, pictures);
  }
}

} // namespace avkit_preview
