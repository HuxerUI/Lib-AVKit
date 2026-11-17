#pragma once

#include <memory>
#include <string_view>

#include <huxerui/external_texture.h>

namespace avkit_preview {

void ConfigureFilePreview(std::string_view input, std::string_view start_seconds);
std::shared_ptr<huxerui::ExternalTexture> CreateFilePreview();
bool HasFilePreview();
void SeekFilePreview(bool midpoint);
void StopFilePreview();
void ExportFilePreview(std::string_view input, std::string_view output, bool software_only);
void VerifyFilePreview(std::string_view input, std::string_view output);

} // namespace avkit_preview
