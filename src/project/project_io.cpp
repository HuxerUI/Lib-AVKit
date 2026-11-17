#include "project_internal.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

#include "support/staged_output.h"

namespace huxerui::avkit {
namespace {

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

void RequireIo(IoResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

} // namespace

void Project::Save(const File& destination) const {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<float>::max_digits10);
  const auto& project = *current_;
  const bool versioned_assets = std::ranges::any_of(project.assets, [](const auto& asset) { return asset.content_revision != 0; });
  const auto any_clip = [&](auto predicate) {
    return std::ranges::any_of(project.tracks, [&](const auto& track) {
      return std::ranges::any_of(track.clips, predicate);
    });
  };
  const bool reversed = any_clip([](const auto& clip) { return clip.placement.source.reverse; });
  const bool tracked = any_clip([](const auto& clip) {
    return std::ranges::any_of(clip.stickers, [](const auto& sticker) { return sticker.spec.anchor.has_value(); });
  });
  const int version = tracked ? 4 : versioned_assets || reversed ? 3 : project.transitions.empty() ? 1 : 2;
  output << "AVKIT_PROJECT " << version << '\n';
  output << "canvas " << project.canvas.width << ' ' << project.canvas.height << '\n';
  output << "revision " << project.revision << ' ' << project.next_id << '\n';
  output << "assets " << project.assets.size() << '\n';
  for (const auto& asset : project.assets) {
    output << "asset " << asset.id.value << ' ' << std::quoted(asset.source.file.Path()) << ' ' << asset.source.tracks.size();
    if (version >= 3) output << ' ' << asset.content_revision;
    output << '\n';
    for (const auto& track : asset.source.tracks) {
      output << "source " << track.id << ' ' << Kind(track.kind) << ' ';
      Put(output, track.presentation);
      output << '\n';
    }
  }
  output << "tracks " << project.tracks.size() << '\n';
  for (const auto& track : project.tracks) {
    output << "track " << track.id.value << ' ' << Kind(track.kind) << ' ' << track.clips.size() << '\n';
    for (const auto& clip : track.clips) {
      const auto& placement = clip.placement;
      output << "clip " << clip.id.value << ' ' << placement.source.asset.value << ' ' << placement.source.track << ' ';
      Put(output, placement.source.range);
      output << ' ';
      Put(output, placement.timeline);
      output << ' ';
      Put(output, clip.destination);
      output << ' ' << clip.stickers.size();
      if (version >= 3) output << ' ' << placement.source.reverse;
      output << '\n';
      for (const auto& sticker : clip.stickers) {
        const auto& resource = sticker.spec.resource;
        output << "sticker " << sticker.id.value << ' ' << std::quoted(resource.pack) << ' '
               << std::quoted(resource.revision) << ' ' << std::quoted(resource.key) << ' ';
        Put(output, sticker.spec.destination);
        if (version >= 4) {
          const auto& anchor = sticker.spec.anchor;
          output << ' ' << anchor.has_value();
          if (anchor) {
            output << ' ' << static_cast<int>(anchor->part) << ' ' << std::quoted(anchor->point)
                   << ' ' << std::quoted(anchor->axis_point.value_or(""));
          }
        }
        output << '\n';
      }
    }
  }
  if (version >= 2) {
    output << "transitions " << project.transitions.size() << '\n';
    for (const auto& transition : project.transitions) {
      const auto& spec = transition.spec;
      output << "transition " << transition.id.value << ' ' << transition.from.value << ' ' << transition.to.value << ' ';
      Put(output, spec.range);
      output << ' ' << std::quoted(spec.mask.pack) << ' ' << std::quoted(spec.mask.revision) << ' '
             << std::quoted(spec.mask.key) << ' ' << spec.feather << ' ' << spec.inverted << ' ' << spec.feather_keys.size() << '\n';
      for (const auto& key : spec.feather_keys) {
        output << "feather ";
        Put(output, key.time);
        output << ' ' << key.value << ' ' << (key.interpolation == Interpolation::Hold ? 'h' : 'l') << '\n';
      }
    }
  }
  output << "end\n";
  if (!output) throw std::runtime_error("Cannot serialize the project");
  for (const auto& asset : project.assets) {
    if (asset.source.file == destination) throw std::invalid_argument("A project document cannot replace an asset");
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
  const auto version = Read<unsigned>(input);
  if (version < 1 || version > 4) throw std::runtime_error("Unsupported project document version");
  ProjectSnapshot project;
  Tag(input, "canvas");
  project.canvas = {Read<std::uint32_t>(input), Read<std::uint32_t>(input)};
  Tag(input, "revision");
  project.revision = Read<std::uint64_t>(input);
  project.next_id = Read<std::uint64_t>(input);
  Tag(input, "assets");
  for (auto count = Read<std::size_t>(input); count; --count) {
    Tag(input, "asset");
    AssetInfo asset{{Read<std::uint64_t>(input)}, {File(Text(input)), {}}};
    const auto track_count = Read<std::size_t>(input);
    if (version >= 3) asset.content_revision = Read<std::uint64_t>(input);
    for (auto tracks = track_count; tracks; --tracks) {
      Tag(input, "source");
      asset.source.tracks.push_back({Read<std::uint32_t>(input), ReadKind(input), ReadRange(input)});
    }
    project.assets.push_back(std::move(asset));
  }
  Tag(input, "tracks");
  for (auto count = Read<std::size_t>(input); count; --count) {
    Tag(input, "track");
    TrackInfo track{{Read<std::uint64_t>(input)}, ReadKind(input), {}};
    for (auto clips = Read<std::size_t>(input); clips; --clips) {
      Tag(input, "clip");
      ClipInfo clip{{Read<std::uint64_t>(input)},
          {{{Read<std::uint64_t>(input)}, Read<std::uint32_t>(input), ReadRange(input)}, ReadRange(input)},
          ReadRect(input), {}};
      const auto sticker_count = Read<std::size_t>(input);
      if (version >= 3) clip.placement.source.reverse = Read<bool>(input);
      for (auto stickers = sticker_count; stickers; --stickers) {
        Tag(input, "sticker");
        StickerInstance sticker{{Read<std::uint64_t>(input)}, {{Text(input), Text(input), Text(input)}, ReadRect(input)}};
        if (version >= 4 && Read<bool>(input)) {
          const auto part = static_cast<analysis::TrackedPart>(Read<int>(input));
          auto point = Text(input);
          auto axis = Text(input);
          sticker.spec.anchor = {part, std::move(point), axis.empty() ? std::nullopt : std::optional{std::move(axis)}};
        }
        clip.stickers.push_back(std::move(sticker));
      }
      track.clips.push_back(std::move(clip));
    }
    project.tracks.push_back(std::move(track));
  }
  if (version >= 2) {
    Tag(input, "transitions");
    for (auto count = Read<std::size_t>(input); count; --count) {
      Tag(input, "transition");
      TransitionInfo transition{{Read<std::uint64_t>(input)}, {Read<std::uint64_t>(input)}, {Read<std::uint64_t>(input)},
          {ReadRange(input), {Text(input), Text(input), Text(input)}, Read<float>(input), Read<bool>(input), {}}};
      for (auto keys = Read<std::size_t>(input); keys; --keys) {
        Tag(input, "feather");
        const auto at = ReadTime(input);
        const auto value = Read<float>(input);
        const auto interpolation = Read<char>(input);
        if (interpolation != 'h' && interpolation != 'l') throw std::runtime_error("Invalid transition interpolation");
        transition.spec.feather_keys.push_back({at, value, interpolation == 'h' ? Interpolation::Hold : Interpolation::Linear});
      }
      project.transitions.push_back(std::move(transition));
    }
  }
  Tag(input, "end");
  if ((input >> std::ws).peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected project data");
  return Project(std::move(project));
}

} // namespace huxerui::avkit
