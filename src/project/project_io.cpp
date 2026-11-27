#include "project_internal.h"

#include <huxerui/avkit/resource_catalog.h>

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

#include "support/staged_output.h"

namespace huxerui::avkit {
namespace {

constexpr unsigned kProjectFormatVersion = 1;

using media::TrackKind;

template<class T>
T Read(std::istream& input) {
  T value{};
  if (!(input >> value)) throw std::runtime_error("Invalid project document");
  return value;
}

void Tag(std::istream& input, std::string_view tag) {
  if (Read<std::string>(input) != tag) throw std::runtime_error("Invalid project record");
}

std::string Text(std::istream& input) {
  std::string value;
  if (!(input >> std::ws) || input.peek() != '"' || !(input >> std::quoted(value)))
    throw std::runtime_error("Invalid quoted project string");
  return value;
}

TrackKind ReadKind(std::istream& input) {
  const auto kind = Read<char>(input);
  if (kind == 'v') return TrackKind::Video;
  if (kind == 'a') return TrackKind::Audio;
  throw std::runtime_error("Invalid project track kind");
}

char Kind(TrackKind kind) { return kind == TrackKind::Video ? 'v' : 'a'; }

media::MediaTimestamp ReadTime(std::istream& input) { return {Read<std::int64_t>(input), Read<std::uint32_t>(input)}; }

TimeRange ReadRange(std::istream& input) { return {ReadTime(input), ReadTime(input)}; }

void Put(std::ostream& output, media::MediaTimestamp value) { output << value.ticks << ' ' << value.timescale; }

void Put(std::ostream& output, TimeRange value) {
  Put(output, value.start);
  output << ' ';
  Put(output, value.end);
}

Rect ReadRect(std::istream& input) {
  return {Read<float>(input), Read<float>(input), Read<float>(input), Read<float>(input)};
}

void Put(std::ostream& output, Rect value) {
  output << value.x << ' ' << value.y << ' ' << value.width << ' ' << value.height;
}

Color ReadColor(std::istream& input) {
  return {Read<float>(input), Read<float>(input), Read<float>(input), Read<float>(input)};
}

void Put(std::ostream& output, Color color) {
  output << color.red << ' ' << color.green << ' ' << color.blue << ' ' << color.alpha;
}

ResourceRef ReadResource(std::istream& input) { return {Text(input)}; }

void Put(std::ostream& output, const ResourceRef& resource) {
  output << std::quoted(resource.key);
}

void PutBackground(std::ostream& output, const BackgroundReplacementSpec& effect) {
  Put(output, effect.color);
  if (const auto* model = std::get_if<PortraitMask>(&effect.mask)) {
    output << " p " << std::quoted(model->model_id);
  } else if (const auto* mask = std::get_if<StaticMask>(&effect.mask)) {
    output << " s ";
    Put(output, mask->image.resource);
    output << ' ' << (mask->image.channel == MaskChannel::Alpha ? 'a' : 'g') << ' ' << mask->inverted;
  } else {
    const auto& animation = std::get<AnimatedMask>(effect.mask);
    output << " a ";
    Put(output, animation.resource);
    output << ' ' << (animation.playback == AnimationPlayback::Loop ? 'l' : 'h') << ' ' << animation.inverted;
  }
}

BackgroundReplacementSpec ReadBackground(std::istream& input) {
  BackgroundReplacementSpec effect{ReadColor(input)};
  switch (Read<char>(input)) {
    case 'p': effect.mask = PortraitMask{Text(input)}; break;
    case 's': {
      auto resource = ReadResource(input);
      const auto channel = Read<char>(input);
      if (channel != 'g' && channel != 'a') throw std::runtime_error("Invalid static mask channel");
      effect.mask = StaticMask{{std::move(resource), channel == 'a' ? MaskChannel::Alpha : MaskChannel::Grayscale},
                               Read<bool>(input)};
      break;
    }
    case 'a': {
      auto resource = ReadResource(input);
      const auto playback = Read<char>(input);
      if (playback != 'h' && playback != 'l') throw std::runtime_error("Invalid animated mask playback mode");
      effect.mask = AnimatedMask{std::move(resource), playback == 'l' ? AnimationPlayback::Loop : AnimationPlayback::HoldLast,
                                 Read<bool>(input)};
      break;
    }
    default: throw std::runtime_error("Invalid background mask source");
  }
  return effect;
}

void PutReplacements(std::ostream& output, const PagReplacements& replacements) {
  output << ' ' << replacements.texts.size();
  for (const auto& text : replacements.texts) {
    output << ' ' << std::quoted(text.slot) << ' ' << text.text.has_value();
    if (text.text) output << ' ' << std::quoted(*text.text);
    output << ' ' << text.font.has_value();
    if (text.font) { output << ' '; Put(output, *text.font); }
    output << ' ' << text.size_scale.has_value();
    if (text.size_scale) output << ' ' << *text.size_scale;
    for (const auto& color : {text.foreground, text.background}) {
      output << ' ' << color.has_value();
      if (color) { output << ' '; Put(output, *color); }
    }
  }
  output << ' ' << replacements.images.size();
  for (const auto& image : replacements.images) {
    output << ' ' << std::quoted(image.slot) << ' ';
    Put(output, image.image);
  }
}

PagReplacements ReadReplacements(std::istream& input) {
  PagReplacements replacements;
  for (auto count = Read<std::size_t>(input); count; --count) {
    PagTextReplacement text{Text(input)};
    if (Read<bool>(input)) text.text = Text(input);
    if (Read<bool>(input)) text.font = ReadResource(input);
    if (Read<bool>(input)) text.size_scale = Read<float>(input);
    if (Read<bool>(input)) text.foreground = ReadColor(input);
    if (Read<bool>(input)) text.background = ReadColor(input);
    replacements.texts.push_back(std::move(text));
  }
  for (auto count = Read<std::size_t>(input); count; --count)
    replacements.images.push_back({Text(input), ReadResource(input)});
  return replacements;
}

void Put(std::ostream& output, const OverlayContent& content) {
  if (const auto* still = std::get_if<StillImageSource>(&content)) {
    output << "s ";
    Put(output, still->resource);
  } else {
    const auto& pag = std::get<PagOverlay>(content);
    output << "p ";
    Put(output, pag.resource);
    output << ' ' << (pag.playback == AnimationPlayback::Loop ? 'l' : 'h');
    PutReplacements(output, pag.replacements);
  }
}

OverlayContent ReadOverlayContent(std::istream& input) {
  const auto kind = Read<char>(input);
  if (kind != 's' && kind != 'p') throw std::runtime_error("Invalid overlay content");
  auto resource = ReadResource(input);
  if (kind == 's') return StillImageSource{std::move(resource)};
  const auto playback = Read<char>(input);
  if (playback != 'l' && playback != 'h') throw std::runtime_error("Invalid overlay animation playback mode");
  return PagOverlay{std::move(resource), ReadReplacements(input),
                    playback == 'l' ? AnimationPlayback::Loop : AnimationPlayback::HoldLast};
}

void RequireIo(IoResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

} // namespace

void Project::Save(const File& destination) const {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<float>::max_digits10);
  const auto& project = *current_;
  output << "AVKIT_PROJECT " << kProjectFormatVersion << '\n';
  output << "canvas " << project.canvas_size.width << ' ' << project.canvas_size.height << '\n';
  output << "revision " << project.revision << ' ' << project.next_id << '\n';
  const auto media = project.resources->Media();
  output << "assets " << media.size() << '\n';
  for (const auto& [key, asset] : media) {
    output << "asset ";
    Put(output, key);
    output << ' ' << std::quoted(asset->file.Path()) << ' ' << asset->tracks.size() << ' ' << asset->still_image << '\n';
    for (const auto& track : asset->tracks) {
      output << "source " << track.id << ' ' << Kind(track.kind) << ' ';
      Put(output, track.presentation);
      const auto size = track.display_size.value_or(PixelSize{});
      output << ' ' << size.width << ' ' << size.height;
      output << '\n';
    }
  }
  output << "tracks " << project.tracks.size() << '\n';
  for (const auto& track : project.tracks) {
    output << "track " << track.id.value << ' ' << Kind(track.kind) << ' ' << track.clips.size() << '\n';
    for (const auto& clip : track.clips) {
      const auto& placement = clip.placement;
      output << "clip " << clip.id.value << ' ';
      Put(output, placement.source.asset);
      output << ' ' << placement.source.track << ' ';
      Put(output, placement.source.range);
      output << ' ';
      Put(output, placement.timeline);
      output << ' ';
      Put(output, clip.destination);
      output << ' ' << clip.stickers.size() << ' ' << placement.source.reverse << ' ' << clip.background.has_value();
      if (clip.background) {
        output << ' ';
        PutBackground(output, *clip.background);
      }
      output << ' ' << clip.source_crop.has_value();
      if (const auto& crop = clip.source_crop) {
        output << ' ';
        Put(output, crop->area);
        output << ' ' << crop->mirrored;
      }
      output << ' ' << clip.audio_of.value_or(ClipId{}).value << ' ' << clip.audio_mix.gain << ' '
             << clip.audio_mix.muted << ' ';
      Put(output, clip.audio_mix.fade_in);
      output << ' ';
      Put(output, clip.audio_mix.fade_out);
      output << ' ' << clip.fill << ' ' << clip.rotation_degrees << ' '
             << (clip.voice_effect ? static_cast<int>(*clip.voice_effect) : 0) << '\n';
      for (const auto& sticker : clip.stickers) {
        output << "sticker " << sticker.id.value << ' ';
        Put(output, sticker.content);
        output << ' ';
        Put(output, sticker.destination);
        const auto& anchor = sticker.anchor;
        output << ' ' << anchor.has_value();
        if (anchor) {
          output << ' ' << static_cast<int>(anchor->part) << ' ' << std::quoted(anchor->point)
                 << ' ' << std::quoted(anchor->axis_point.value_or(""));
        }
        output << ' ' << sticker.depth.has_value();
        if (sticker.depth) output << ' ' << sticker.depth->meters;
        output << ' ' << (sticker.range != TimeRange{});
        if (sticker.range != TimeRange{}) {
          output << ' ';
          Put(output, sticker.range);
        }
        output << ' ' << static_cast<unsigned>(sticker.kind) << ' ' << sticker.opacity << ' ' << sticker.rotation_degrees << '\n';
      }
    }
  }
  output << "transitions " << project.transitions.size() << '\n';
  for (const auto& transition : project.transitions) {
    const auto& spec = transition.spec;
    output << "transition " << transition.id.value << ' ' << transition.from.value << ' ' << transition.to.value << ' ';
    Put(output, spec.range);
    if (const auto* mask = std::get_if<MaskTransition>(&spec.effect)) {
      output << " m ";
      Put(output, mask->resource);
      output << ' ' << mask->feather << ' ' << mask->inverted << ' ' << mask->feather_keys.size() << '\n';
      for (const auto& key : mask->feather_keys) {
        output << "feather ";
        Put(output, key.time);
        output << ' ' << key.value << ' ' << (key.interpolation == Interpolation::Hold ? 'h' : 'l') << '\n';
      }
    } else {
      output << " p ";
      Put(output, std::get<PagTransitionSource>(spec.effect).resource);
      output << '\n';
    }
  }
  output << "overlays " << project.overlays.size() << '\n';
  for (const auto& overlay : project.overlays) {
    output << "overlay " << overlay.id.value << ' ' << static_cast<unsigned>(overlay.kind) << ' ';
    Put(output, overlay.range);
    output << ' ';
    Put(output, overlay.destination);
    output << ' ';
    Put(output, overlay.content);
    output << ' ' << overlay.opacity << ' ' << overlay.rotation_degrees;
    output << '\n';
  }
  output << "end\n";
  if (!output) throw std::runtime_error("Cannot serialize the project");
  for (const auto& [key, asset] : media) {
    if (asset->file == destination) throw std::invalid_argument("A project document cannot replace an asset");
  }
  const auto serialized = output.str();
  detail::StagedOutput stage(destination);
  auto stream = stage.TakeStream();
  RequireIo(stream.WriteAt(0, std::as_bytes(std::span(serialized))));
  RequireIo(stream.Close());
  stage.Publish(destination, true);
}

Project Project::Open(const File& source) {
  auto text = source.ReadString();
  if (!text.Succeeded()) throw std::runtime_error(text.Error().message);
  std::istringstream input(std::move(text).Value());
  input.imbue(std::locale::classic());
  Tag(input, "AVKIT_PROJECT");
  if (Read<unsigned>(input) != kProjectFormatVersion) throw std::runtime_error("Unsupported project document version");
  ProjectSnapshot project;
  Tag(input, "canvas");
  project.canvas_size = {Read<std::uint32_t>(input), Read<std::uint32_t>(input)};
  Tag(input, "revision");
  project.revision = Read<std::uint64_t>(input);
  project.next_id = Read<std::uint64_t>(input);
  Tag(input, "assets");
  auto resources = std::make_shared<ResourceCatalog>();
  for (auto count = Read<std::size_t>(input); count; --count) {
    Tag(input, "asset");
    auto resource = ReadResource(input);
    AssetSource asset{File(Text(input)), {}};
    const auto track_count = Read<std::size_t>(input);
    asset.still_image = Read<bool>(input);
    for (auto tracks = track_count; tracks; --tracks) {
      Tag(input, "source");
      AssetTrack track{Read<std::uint32_t>(input), ReadKind(input), ReadRange(input)};
      const PixelSize size{Read<std::uint32_t>(input), Read<std::uint32_t>(input)};
      if (size.width || size.height) track.display_size = size;
      asset.tracks.push_back(track);
    }
    resources->Register(std::move(resource), std::move(asset));
  }
  project.resources = std::move(resources);
  Tag(input, "tracks");
  for (auto count = Read<std::size_t>(input); count; --count) {
    Tag(input, "track");
    TimelineTrack track{{Read<std::uint64_t>(input)}, ReadKind(input), {}};
    for (auto clips = Read<std::size_t>(input); clips; --clips) {
      Tag(input, "clip");
      TimelineClip clip{{Read<std::uint64_t>(input)},
          {{ReadResource(input), Read<std::uint32_t>(input), ReadRange(input)}, ReadRange(input)},
          ReadRect(input), {}};
      const auto sticker_count = Read<std::size_t>(input);
      clip.placement.source.reverse = Read<bool>(input);
      if (Read<bool>(input)) clip.background = ReadBackground(input);
      if (Read<bool>(input)) clip.source_crop = SourceCrop{ReadRect(input), Read<bool>(input)};
      const auto owner = Read<std::uint64_t>(input);
      if (owner) clip.audio_of = ClipId{owner};
      clip.audio_mix = {Read<float>(input), Read<bool>(input), ReadTime(input), ReadTime(input)};
      clip.fill = Read<bool>(input);
      clip.rotation_degrees = Read<float>(input);
      const auto voice = Read<int>(input);
      if (voice == static_cast<int>(VoiceEffect::Robot)) clip.voice_effect = VoiceEffect::Robot;
      else if (voice != 0) throw std::runtime_error("Invalid voice effect");
      for (auto stickers = sticker_count; stickers; --stickers) {
        Tag(input, "sticker");
        OverlayInstance sticker{.id = {Read<std::uint64_t>(input)}};
        sticker.content = ReadOverlayContent(input);
        sticker.destination = ReadRect(input);
        if (Read<bool>(input)) {
          const auto part = static_cast<analysis::TrackedPart>(Read<int>(input));
          auto point = Text(input);
          auto axis = Text(input);
          sticker.anchor = {part, std::move(point), axis.empty() ? std::nullopt : std::optional{std::move(axis)}};
        }
        if (Read<bool>(input)) sticker.depth = StickerDepth{Read<float>(input)};
        if (Read<bool>(input)) sticker.range = ReadRange(input);
        sticker.kind = static_cast<CanvasOverlayKind>(Read<unsigned>(input));
        sticker.opacity = Read<float>(input);
        sticker.rotation_degrees = Read<float>(input);
        clip.stickers.push_back(std::move(sticker));
      }
      track.clips.push_back(std::move(clip));
    }
    project.tracks.push_back(std::move(track));
  }
  Tag(input, "transitions");
  for (auto count = Read<std::size_t>(input); count; --count) {
    Tag(input, "transition");
    TransitionInstance transition{{Read<std::uint64_t>(input)}, {Read<std::uint64_t>(input)}, {Read<std::uint64_t>(input)},
                                  {ReadRange(input)}};
    const auto kind = Read<char>(input);
    if (kind != 'm' && kind != 'p') throw std::runtime_error("Invalid transition effect");
    auto resource = ReadResource(input);
    if (kind == 'p') {
      transition.spec.effect = PagTransitionSource{std::move(resource)};
    } else {
      MaskTransition mask{std::move(resource), Read<float>(input), Read<bool>(input)};
      for (auto keys = Read<std::size_t>(input); keys; --keys) {
        Tag(input, "feather");
        const auto at = ReadTime(input);
        const auto value = Read<float>(input);
        const auto interpolation = Read<char>(input);
        if (interpolation != 'h' && interpolation != 'l') throw std::runtime_error("Invalid transition interpolation");
        mask.feather_keys.push_back({at, value, interpolation == 'h' ? Interpolation::Hold : Interpolation::Linear});
      }
      transition.spec.effect = std::move(mask);
    }
    project.transitions.push_back(std::move(transition));
  }
  Tag(input, "overlays");
  for (auto count = Read<std::size_t>(input); count; --count) {
    Tag(input, "overlay");
    OverlayInstance overlay{{Read<std::uint64_t>(input)}, static_cast<CanvasOverlayKind>(Read<unsigned>(input)),
                          ReadRange(input), ReadRect(input)};
    overlay.content = ReadOverlayContent(input);
    overlay.opacity = Read<float>(input);
    overlay.rotation_degrees = Read<float>(input);
    project.overlays.push_back(std::move(overlay));
  }
  Tag(input, "end");
  if ((input >> std::ws).peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected project data");
  return Project(std::move(project));
}

} // namespace huxerui::avkit
