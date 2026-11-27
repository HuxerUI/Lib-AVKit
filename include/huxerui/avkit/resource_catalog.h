#pragma once

/// @file
/// @brief Resource inputs shared by projects, scene evaluation, and rendering.

#include <array>
#include <concepts>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/avkit/edit_template.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>

namespace huxerui::avkit {

/// @brief Maps a logical PAG slot name to the authored editable layer index.
struct PagEditableSlot {
  std::string name;
  int editable_index;
};

/// @brief Editable features of a PAG composition stored under the same resource key.
struct PagAsset {
  /// Editable text slots addressable by name.
  std::vector<PagEditableSlot> text_slots;
  /// Editable image slots addressable by name.
  std::vector<PagEditableSlot> image_slots;
  /// Optional default font key in the same catalog.
  std::optional<ResourceRef> font;
  /// Outgoing and incoming image slots when used as a transition.
  std::optional<std::array<std::string, 2>> transition_slots;
  /// Fit the sole editable text slot to the instance bounds at the output resolution.
  bool fit_text_to_canvas{};
};

/// @brief Base of the immutable records stored in the catalog.
struct ResourceData {
  virtual ~ResourceData() = default;
};

/// @brief Ordered coverage frames sampled by a clip effect or transition.
struct MaskSequenceData final : ResourceData {
  MaskSequenceData() = default;

  /// @param images Image references and coverage channels in presentation order.
  /// @param interval Frame interval for clip playback. Use zero for a transition-only sequence.
  explicit MaskSequenceData(std::vector<MaskImage> images, media::MediaTimestamp interval = {});

  /// Image references and channels in presentation order.
  std::vector<MaskImage> frames;
  /// Positive interval for clip playback. A zero interval is valid for transition-only sequences.
  media::MediaTimestamp frame_interval{};
};

/// @brief Registers immutable resource records before publication.
/// Share a const catalog while sessions or render workers use it. Publish a new catalog when assets change.
class ResourceCatalog {
public:
  /// @brief Registers a local file without opening it.
  /// @param reference Key unique across all resource types.
  /// @param file Local file. Keep its contents stable while a catalog snapshot uses it.
  void Register(ResourceRef reference, File file);

  /// @brief Registers inspected media for use on one or more timeline tracks.
  /// @param reference Key unique across all resource types.
  /// @param source Local file and source track metadata.
  void Register(ResourceRef reference, AssetSource source);

  /// @brief Updates the file location of existing media in this catalog.
  /// @param reference Key of registered media.
  /// @param file New location of the original media.
  void Relink(const ResourceRef& reference, File file);

  /// @brief Returns the immutable record retained by readers or caches.
  /// @param reference Key of registered media.
  /// @return Empty for an unknown key or a resource of a different type.
  std::shared_ptr<const AssetSource> FindMedia(const ResourceRef& reference) const;

  /// @brief Returns all media records in registration order.
  std::vector<std::pair<ResourceRef, std::shared_ptr<const AssetSource>>> Media() const;

  /// @brief Reports whether the catalog has any registered resources.
  bool Empty() const noexcept { return entries_.empty(); }

  /// @brief Returns a session catalog sharing the immutable records from both catalogs.
  /// @param project Project media. Its media records supersede file entries with the same key.
  ResourceCatalog WithMedia(const ResourceCatalog& project) const;

  /// @brief Registers packaged or memory-backed content without reading it.
  /// @param reference Key unique in the catalog.
  /// @param asset Retained resource. Keep the SDK resource source alive for lazy reads.
  void Register(ResourceRef reference, RawAsset asset);

  /// @brief Registers a local PAG file and its metadata under one key.
  /// @param reference Key unique in the catalog.
  /// @param file Local PAG file.
  /// @param metadata Editable slots and font references for this revision.
  void Register(ResourceRef reference, File file, PagAsset metadata);

  /// @brief Registers a packaged PAG file and its metadata under one key.
  /// @param reference Key unique in the catalog.
  /// @param asset Retained PAG bytes or an SDK raw resource.
  /// @param metadata Editable slots and font references for this revision.
  void Register(ResourceRef reference, RawAsset asset, PagAsset metadata);

  /// @brief Registers an ordered set of separately readable mask images.
  /// @param reference Catalog key of the sequence.
  /// @param sequence Frame references, channels, and timing.
  void Register(ResourceRef reference, MaskSequenceData sequence);

  /// @brief Registers an edit template with the supplied resource identity.
  /// @param reference Catalog key of the template.
  /// @param recipe Template tracks, resource references, and assembly rules.
  void Register(ResourceRef reference, EditTemplateRecipe recipe);

  /// @brief Registers an extracted file after verifying it is inside the pack directory.
  /// @param reference Key unique in the catalog.
  /// @param root Existing pack directory.
  /// @param relative_path File path relative to the pack directory.
  /// @throws std::invalid_argument If the path escapes the pack or does not name a regular file.
  void Register(ResourceRef reference, const File& root, std::string_view relative_path);

  /// @brief Registers an extracted PAG file and its metadata under one key.
  /// @param reference Key unique in the catalog.
  /// @param root Existing pack directory.
  /// @param relative_path PAG file path relative to the pack directory.
  /// @param metadata Editable slots and font references for this revision.
  void Register(ResourceRef reference, const File& root, std::string_view relative_path, PagAsset metadata);

  /// @brief Opens an independent input stream for a readable resource.
  /// @param reference Exact identity of a plain or PAG file.
  /// @return A stream positioned at byte zero.
  /// @throws std::invalid_argument If the reference does not name readable content.
  /// @throws std::runtime_error On a file open failure.
  InputStream OpenRead(const ResourceRef& reference) const;

  /// @brief Looks up the local path stored with a readable resource without opening it.
  /// @param reference Exact resource identity.
  /// @return The path when available. Empty for bundled, structured, or missing resources.
  std::optional<File> FindLocalFile(const ResourceRef& reference) const;

  /// @brief Looks up typed metadata without opening the resource payload.
  /// @tparam T Metadata type registered for the resource.
  /// @param reference Exact resource identity.
  /// @return Metadata borrowed from the catalog, or null when missing or of another type.
  template<class T>
    requires (std::same_as<T, PagAsset> || std::same_as<T, MaskSequenceData> || std::same_as<T, EditTemplateRecipe>)
  const T* Find(const ResourceRef& reference) const {
    const auto* data = Lookup(reference);
    if constexpr (std::same_as<T, PagAsset>) {
      const auto* entry = dynamic_cast<const PagData*>(data);
      return entry ? &entry->metadata : nullptr;
    } else if constexpr (std::same_as<T, MaskSequenceData>) {
      return dynamic_cast<const MaskSequenceData*>(data);
    } else {
      const auto* entry = dynamic_cast<const EditTemplateData*>(data);
      return entry ? &entry->metadata : nullptr;
    }
  }

  /// @brief Returns the requested metadata.
  /// @tparam T Metadata type registered for the resource.
  /// @param reference Exact resource identity.
  /// @return Metadata borrowed from the catalog.
  /// @throws std::invalid_argument If the resource is missing or has a different metadata type.
  template<class T> const T& Get(const ResourceRef& reference) const {
    if (const auto* metadata = Find<T>(reference)) return *metadata;
    throw std::invalid_argument("Missing or incompatible resource: " + reference.key);
  }

private:
  /// @brief Readable content supplied by a local file or a retained raw asset.
  struct FileData final : ResourceData {
    explicit FileData(File source);
    explicit FileData(RawAsset source);

    std::optional<File> file;
    RawAsset asset;
  };

  /// @brief Local media and track metadata retained as one immutable catalog record.
  struct MediaData final : ResourceData {
    explicit MediaData(AssetSource value);

    AssetSource source;
  };

  /// @brief Readable PAG content and the editable metadata of the same revision.
  struct PagData final : ResourceData {
    PagData(File source, PagAsset value);
    PagData(RawAsset source, PagAsset value);

    FileData content;
    PagAsset metadata;
  };

  /// @brief Edit template recipe stored under its catalog key.
  struct EditTemplateData final : ResourceData {
    explicit EditTemplateData(EditTemplateRecipe value);

    EditTemplateRecipe metadata;
  };

  void Insert(ResourceRef reference, std::shared_ptr<const ResourceData> data);
  const ResourceData* Lookup(const ResourceRef& reference) const;
  const FileData* FindFileData(const ResourceRef& reference) const;

  std::map<ResourceRef, std::shared_ptr<const ResourceData>> entries_;
  std::vector<ResourceRef> media_;
};

} // namespace huxerui::avkit
