#include <huxerui/app.h>

#include <string_view>

#include "capture_preview.h"
#include "metal_preview.h"
#include "workbench_media.h"

int main(int argc, const char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--verify") {
    return avkit_preview::VerifyMetalPreview();
  }
  if (const auto result = avkit_preview::RunCaptureCommand(argc, argv)) return *result;
  if (const auto result = avkit_preview::RunFileMediaCommand(argc, argv)) return *result;
  return huxerui::RunApplication();
}
