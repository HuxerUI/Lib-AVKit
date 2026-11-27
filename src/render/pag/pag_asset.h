#pragma once

/// @file
/// @brief Loads PAG assets and resolves composition replacements for rendering.

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <huxerui/avkit/resource_catalog.h>
#include <pag/pag.h>

namespace huxerui::avkit::detail {

/// @brief Registers the first face in a packaged font for use by PAG text layers. Link AVKit::PAG.
/// Share a font between animations using the same resource. The last owner releases the registration.
class PagFont {
public:
  /// @param catalog Catalog supplying the font payload.
  /// @param source Exact resource identity of the font.
  /// @throws std::invalid_argument If the source does not contain a supported font.
  PagFont(const ResourceCatalog& catalog, const ResourceRef& source);

  ~PagFont();

  /// @brief Family registered for this packaged font on its owning sequence.
  const std::string& Family() const noexcept { return family_; }

  PagFont(const PagFont&) = delete;

  PagFont& operator=(const PagFont&) = delete;

private:
  Bytes packaged_data_;
  std::string family_;
};

/// @brief Resolved text slot in one version of a PAG resource.
struct IndexedPagTextReplacement {
  int editable_index;
  /// Omit to keep the authored text while applying the packaged font.
  std::optional<std::string> text;
  std::shared_ptr<const PagFont> font;
  std::optional<float> size_scale;
  std::optional<Color> foreground;
  std::optional<Color> background;
};

/// @brief Resolved image slot in one version of a PAG resource.
struct IndexedPagImageReplacement {
  int editable_index;
  ResourceRef resource;
};

/// @brief Loads the original PAG file from its resource contents.
/// @param catalog Catalog holding the PAG payload.
/// @param resource Exact resource identity of the PAG composition.
std::shared_ptr<pag::PAGFile> LoadPagFile(const ResourceCatalog& catalog, const ResourceRef& resource);

/// @brief Applies supplied text and style fields while preserving authored defaults.
/// @param document Mutable libpag text document.
/// @param value Resolved replacement fields for the document.
void ApplyPagTextReplacement(pag::TextDocument& document, const IndexedPagTextReplacement& value);

struct ResolvedPagAsset {
  ResourceRef resource;
  std::vector<IndexedPagTextReplacement> texts;
  std::vector<IndexedPagImageReplacement> images;
};

/// @brief Loads a composition, validates its editable indices and applies replacements.
/// @param catalog Catalog providing the composition and replacement image payloads.
/// @param asset Exact resource identity and resolved replacement values.
std::shared_ptr<pag::PAGFile> OpenPagAnimation(const ResourceCatalog& catalog, const ResolvedPagAsset& asset);

using PagFontCache = std::map<ResourceRef, std::weak_ptr<const PagFont>>;

/// @brief Resolves an overlay and its replacements against the current resource catalog.
/// @param source Authored resource and instance replacements.
/// @param resources Catalog holding the PAG file, fonts and images.
/// @param fonts Font registrations shared by compositions on the rendering sequence.
/// @param slots Precomputed editable indices. Omit to resolve the named slots here.
ResolvedPagAsset ResolvePagAsset(const PagOverlay& source, const ResourceCatalog& resources, PagFontCache& fonts,
                                 const PagEditableIndices* slots = nullptr);

} // namespace huxerui::avkit::detail
