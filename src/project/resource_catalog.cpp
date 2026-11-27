#include <huxerui/avkit/resource_catalog.h>

#include "project/resource_internal.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace huxerui::avkit {
namespace {

void ValidateRelativePath(std::string_view path) {
  if (path.empty() || path.find_first_of("\\:") != path.npos || path.find('\0') != path.npos)
    throw std::invalid_argument("Invalid pack resource path");
  for (;;) {
    const auto slash = path.find('/');
    const auto name = path.substr(0, slash);
    if (name.empty() || name == "." || name == "..")
      throw std::invalid_argument("Invalid pack resource path");
    if (slash == path.npos) break;
    path.remove_prefix(slash + 1);
  }
}

namespace fs = std::filesystem;

fs::path Utf8Path(std::string_view path) {
  return fs::path(std::u8string(path.begin(), path.end()));
}

File ResolvePackFile(const File& root, std::string_view path) {
  ValidateRelativePath(path);
  std::error_code error;
  const auto directory = fs::canonical(Utf8Path(root.Path()), error);
  if (error || !fs::is_directory(directory, error))
    throw std::invalid_argument("Invalid resource pack directory");
  const auto resolved = fs::canonical(directory / Utf8Path(path), error);
  if (error || !fs::is_regular_file(resolved, error) ||
      std::ranges::mismatch(directory, resolved).in1 != directory.end())
    throw std::invalid_argument("Resource is not a file inside the pack: " + std::string(path));
  return File(resolved.generic_u8string());
}

void ValidatePag(const PagAsset& asset) {
  if (asset.font && asset.font->key.empty()) throw std::invalid_argument("A PAG font needs a resource key");
  if (asset.fit_text_to_canvas && asset.text_slots.size() != 1)
    throw std::invalid_argument("Canvas-fitted PAG text needs one editable text slot");
}

} // namespace

int detail::PagEditableIndex(std::span<const PagEditableSlot> slots, std::string_view name) {
  const auto slot = std::ranges::find(slots, name, &PagEditableSlot::name);
  if (slot == slots.end()) throw std::invalid_argument("Unknown PAG editable slot: " + std::string(name));
  return slot->editable_index;
}

PagEditableIndices detail::ResolvePagIndices(const PagAsset& asset, const PagReplacements& replacements) {
  PagEditableIndices result;
  result.text.reserve(replacements.texts.size());
  result.image.reserve(replacements.images.size());
  for (const auto& text : replacements.texts) result.text.push_back(PagEditableIndex(asset.text_slots, text.slot));
  for (const auto& image : replacements.images) result.image.push_back(PagEditableIndex(asset.image_slots, image.slot));
  return result;
}

Bytes detail::ReadResourceBytes(const ResourceCatalog& catalog, const ResourceRef& reference) {
  auto stream = catalog.OpenRead(reference);
  std::array<std::byte, 64 * 1024> buffer;
  Bytes bytes;
  for (;;) {
    const auto read = stream.Read(buffer);
    if (!read.Succeeded()) throw std::runtime_error(read.Error().message);
    const auto count = read.Value();
    if (!count) return bytes;
    bytes.insert(bytes.end(), buffer.data(), buffer.data() + count);
  }
}

ResourceCatalog::FileData::FileData(File source) : file(std::move(source)) {
  if (file->Path().empty()) throw std::invalid_argument("A resource needs a local file");
}

ResourceCatalog::FileData::FileData(RawAsset source) : asset(std::move(source)) {
  if (!asset.HasValue()) throw std::invalid_argument("A resource needs a raw asset");
}

ResourceCatalog::MediaData::MediaData(AssetSource value) : source(std::move(value)) {
  if (source.file.Path().empty()) throw std::invalid_argument("Media needs a local file");
}

ResourceCatalog::PagData::PagData(File source, PagAsset value)
    : content(std::move(source)), metadata(std::move(value)) {}

ResourceCatalog::PagData::PagData(RawAsset source, PagAsset value)
    : content(std::move(source)), metadata(std::move(value)) {}

MaskSequenceData::MaskSequenceData(std::vector<MaskImage> images, media::MediaTimestamp interval)
    : frames(std::move(images)), frame_interval(interval) {}

ResourceCatalog::EditTemplateData::EditTemplateData(EditTemplateRecipe value) : metadata(std::move(value)) {}

void ResourceCatalog::Register(ResourceRef reference, File file) {
  Insert(std::move(reference), std::make_shared<FileData>(std::move(file)));
}

void ResourceCatalog::Register(ResourceRef reference, AssetSource source) {
  media_.push_back(reference);
  try {
    Insert(std::move(reference), std::make_shared<MediaData>(std::move(source)));
  } catch (...) {
    media_.pop_back();
    throw;
  }
}

void ResourceCatalog::Relink(const ResourceRef& reference, File file) {
  const auto source = FindMedia(reference);
  if (!source || !file.IsFile()) throw std::invalid_argument("Relinking needs registered media and an available file");
  auto record = *source;
  record.file = std::move(file);
  entries_.at(reference) = std::make_shared<MediaData>(std::move(record));
}

std::shared_ptr<const AssetSource> ResourceCatalog::FindMedia(const ResourceRef& reference) const {
  const auto entry = entries_.find(reference);
  if (entry == entries_.end()) return {};
  const auto* media = dynamic_cast<const MediaData*>(entry->second.get());
  if (!media) return {};
  return {entry->second, &media->source};
}

std::vector<std::pair<ResourceRef, std::shared_ptr<const AssetSource>>> ResourceCatalog::Media() const {
  std::vector<std::pair<ResourceRef, std::shared_ptr<const AssetSource>>> result;
  result.reserve(media_.size());
  for (const auto& reference : media_) result.emplace_back(reference, FindMedia(reference));
  return result;
}

ResourceCatalog ResourceCatalog::WithMedia(const ResourceCatalog& project) const {
  auto result = *this;
  for (const auto& reference : project.media_) {
    const auto entry = result.entries_.find(reference);
    if (entry == result.entries_.end() || dynamic_cast<const FileData*>(entry->second.get())) {
      result.media_.push_back(reference);
    } else if (!dynamic_cast<const MediaData*>(entry->second.get())) {
      throw std::invalid_argument("Project media key conflicts with a packaged resource: " + reference.key);
    }
    result.entries_.insert_or_assign(reference, project.entries_.at(reference));
  }
  return result;
}

void ResourceCatalog::Register(ResourceRef reference, RawAsset asset) {
  Insert(std::move(reference), std::make_shared<FileData>(std::move(asset)));
}

void ResourceCatalog::Register(ResourceRef reference, File file, PagAsset metadata) {
  ValidatePag(metadata);
  Insert(std::move(reference), std::make_shared<PagData>(std::move(file), std::move(metadata)));
}

void ResourceCatalog::Register(ResourceRef reference, RawAsset asset, PagAsset metadata) {
  ValidatePag(metadata);
  Insert(std::move(reference), std::make_shared<PagData>(std::move(asset), std::move(metadata)));
}

void ResourceCatalog::Register(ResourceRef reference, MaskSequenceData sequence) {
  if (sequence.frames.empty() || sequence.frames.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("A mask sequence needs an addressable set of frames");
  if (sequence.frame_interval.ticks < 0 || (sequence.frame_interval.ticks && !sequence.frame_interval.timescale))
    throw std::invalid_argument("Invalid mask sequence frame interval");
  for (const auto& frame : sequence.frames) {
    if (frame.resource.key.empty()) throw std::invalid_argument("A mask frame needs a resource key");
    if (frame.channel != MaskChannel::Grayscale && frame.channel != MaskChannel::Alpha)
      throw std::invalid_argument("Invalid mask channel");
  }
  Insert(std::move(reference), std::make_shared<MaskSequenceData>(std::move(sequence)));
}

void ResourceCatalog::Register(ResourceRef reference, EditTemplateRecipe recipe) {
  recipe.reference = reference;
  Insert(std::move(reference), std::make_shared<EditTemplateData>(std::move(recipe)));
}

void ResourceCatalog::Register(ResourceRef reference, const File& root, std::string_view relative_path) {
  Register(std::move(reference), ResolvePackFile(root, relative_path));
}

void ResourceCatalog::Register(ResourceRef reference, const File& root, std::string_view relative_path,
                               PagAsset metadata) {
  Register(std::move(reference), ResolvePackFile(root, relative_path), std::move(metadata));
}

InputStream ResourceCatalog::OpenRead(const ResourceRef& reference) const {
  if (const auto file = FindLocalFile(reference)) {
    auto opened = file->OpenRead();
    if (!opened.Succeeded()) throw std::runtime_error(opened.Error().message);
    return std::move(opened).Value();
  }
  if (const auto* data = FindFileData(reference)) return data->asset.OpenRead();
  throw std::invalid_argument("Resource has no readable content: " + reference.key);
}

std::optional<File> ResourceCatalog::FindLocalFile(const ResourceRef& reference) const {
  if (const auto media = FindMedia(reference)) return media->file;
  const auto* data = FindFileData(reference);
  return data ? data->file : std::nullopt;
}

void ResourceCatalog::Insert(ResourceRef reference, std::shared_ptr<const ResourceData> data) {
  if (reference.key.empty()) throw std::invalid_argument("A registered resource needs a key");
  const auto [entry, inserted] = entries_.try_emplace(std::move(reference), std::move(data));
  if (!inserted) throw std::invalid_argument("Resource is already registered: " + entry->first.key);
}

const ResourceData* ResourceCatalog::Lookup(const ResourceRef& reference) const {
  const auto found = entries_.find(reference);
  return found == entries_.end() ? nullptr : found->second.get();
}

const ResourceCatalog::FileData* ResourceCatalog::FindFileData(const ResourceRef& reference) const {
  const auto* data = Lookup(reference);
  if (const auto* file = dynamic_cast<const FileData*>(data)) return file;
  if (const auto* pag = dynamic_cast<const PagData*>(data)) return &pag->content;
  return nullptr;
}

} // namespace huxerui::avkit
