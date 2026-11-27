#pragma once

#include <huxerui/avkit/resource_catalog.h>

#include <span>
#include <string_view>

namespace huxerui::avkit::detail {

/// @brief Reads the full payload for a decoder that requires contiguous encoded bytes.
/// @param catalog Catalog supplying the readable resource.
/// @param reference Exact resource identity.
/// @return Owned bytes read to EOF. An I/O failure discards partial data.
Bytes ReadResourceBytes(const ResourceCatalog& catalog, const ResourceRef& reference);

int PagEditableIndex(std::span<const PagEditableSlot> slots, std::string_view name);

PagEditableIndices ResolvePagIndices(const PagAsset& asset, const PagReplacements& replacements);

} // namespace huxerui::avkit::detail
