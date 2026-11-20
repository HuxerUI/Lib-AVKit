#include <huxerui/app.h>

#include <string_view>

#if defined(AVKIT_MACOS_PREVIEW)
#include "metal_preview.h"
#endif

#if defined(AVKIT_FILE_MEDIA) || defined(AVKIT_CAPTURE_PREVIEW)
#include <exception>
#include <iostream>
#endif

#if defined(AVKIT_FILE_MEDIA)
#include "file_preview.h"
#include "timeline_preview.h"
#include "timeline_depth_verify.h"
#include "timeline_mask_verify.h"
#include "timeline_strip.h"
#include "timeline_template_mask_verify.h"
#include "timeline_tracking_verify.h"
#if defined(AVKIT_PAG_PREVIEW)
#include "timeline_pag_verify.h"
#endif
#endif
#if defined(AVKIT_CAPTURE_PREVIEW)
#include "capture_preview.h"
#endif

int main(int argc, const char** argv) {
#if defined(AVKIT_MACOS_PREVIEW)
  if (argc == 2 && std::string_view(argv[1]) == "--verify") {
    return avkit_preview::VerifyMetalPreview();
  }
#else
  (void)argc;
  (void)argv;
#endif
#if defined(AVKIT_CAPTURE_PREVIEW)
  if (argc == 3 && (std::string_view(argv[1]) == "--camera-preview" || std::string_view(argv[1]) == "--verify-camera" ||
                    std::string_view(argv[1]) == "--verify-live-capture")) {
    try {
      using enum avkit_preview::CapturePreviewMode;
      const std::string_view command(argv[1]);
      const auto mode = command == "--verify-camera" ? VerifyCamera :
                        command == "--verify-live-capture" ? VerifyLive : Interactive;
      avkit_preview::ConfigureCapturePreview(argv[2], mode);
      return avkit_preview::CapturePreviewExitCode(huxerui::RunApplication());
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
#endif
#if defined(AVKIT_FILE_MEDIA)
  if (argc > 1 && std::string_view(argv[1]).starts_with("--timeline-")) {
    try {
      const std::string_view command(argv[1]);
      if ((command == "--timeline-preview" || command == "--timeline-tracking-preview") && (argc == 3 || argc == 4)) {
        avkit_preview::ConfigureTimelinePreview(argv[2], argc == 4 ? argv[3] : "0",
            command == "--timeline-tracking-preview" ? avkit_preview::CreateMockTrackingProvider() : nullptr);
        const int result = huxerui::RunApplication();
        avkit_preview::StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-export" && (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--software-only"))) {
        avkit_preview::ExportTimelinePreview(argv[2], argv[3], argc == 5);
        return 0;
      }
      if (command == "--timeline-verify" && argc == 5) {
        avkit_preview::VerifyTimelinePreview(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-verify-codecs" && argc == 4) {
        avkit_preview::VerifyTimelineCodecs(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-transition" && argc == 5) {
        avkit_preview::VerifyTimelineTransition(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-assets" && argc == 4) {
        avkit_preview::ConfigureTimelineStrip(argv[2], argv[3]);
        const auto result = huxerui::RunApplication();
        avkit_preview::StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-verify-assets" && argc == 5) {
        avkit_preview::VerifyTimelineStrip(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-verify-tracking" && argc == 3) {
        avkit_preview::VerifyTimelineTracking(argv[2]);
        return 0;
      }
      if (command == "--timeline-verify-masks" && argc == 3) {
        avkit_preview::VerifyTimelineMasks(argv[2]);
        return 0;
      }
      if (command == "--timeline-verify-depth" && argc == 4) {
        avkit_preview::VerifyTimelineDepth(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-template-masks" && argc == 4) {
        avkit_preview::VerifyTimelineTemplateMasks(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-animated-template-masks" && argc == 4) {
        avkit_preview::VerifyTimelineAnimatedTemplateMasks(argv[2], argv[3]);
        return 0;
      }
#if defined(AVKIT_PAG_PREVIEW)
      if (command == "--timeline-verify-pag-stickers" && argc == 4) {
        avkit_preview::VerifyTimelinePagStickers(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-stickers")
        std::cerr << "Usage: --timeline-verify-pag-stickers PROJECT DIRECTORY\n";
      if (command == "--timeline-verify-pag-replacements" && argc == 4) {
        avkit_preview::VerifyTimelinePagReplacements(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-replacements")
        std::cerr << "Usage: --timeline-verify-pag-replacements PROJECT DIRECTORY\n";
      if (command == "--timeline-verify-pag-transition" && argc == 4) {
        avkit_preview::VerifyTimelinePagTransition(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-transition")
        std::cerr << "Usage: --timeline-verify-pag-transition PROJECT DIRECTORY\n";
#endif
      if (command == "--timeline-mask-preview" && (argc == 3 || argc == 4)) {
        avkit_preview::ConfigureTimelinePreview(argv[2], argc == 4 ? argv[3] : "0",
                                                avkit_preview::CreateMockMaskProvider(), true);
        const int result = huxerui::RunApplication();
        avkit_preview::StopTimelinePreview();
        return result;
      }
#if defined(AVKIT_NCNN_PREVIEW)
      if (command == "--timeline-verify-ncnn-mask" &&
          (argc == 5 || (argc == 6 && std::string_view(argv[5]) == "--mixed-models"))) {
        avkit_preview::VerifyTimelineNcnnMasks(argv[2], argv[3], argv[4], argc == 6);
        return 0;
      }
      if (command == "--timeline-ncnn-export" && argc == 5) {
        avkit_preview::ExportTimelinePreview(argv[2], argv[4], true, avkit_preview::CreateTimelineNcnnProvider(argv[3]));
        return 0;
      }
      if (command == "--timeline-ncnn-preview" && (argc == 4 || argc == 5)) {
        avkit_preview::ConfigureTimelinePreview(argv[2], argc == 5 ? argv[4] : "0",
                                                avkit_preview::CreateTimelineNcnnProvider(argv[3]), true);
        const auto result = huxerui::RunApplication();
        avkit_preview::StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-verify-ncnn" && argc == 5) {
        avkit_preview::VerifyTimelineNcnn(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-verify-ncnn-body" && argc == 6) {
        avkit_preview::VerifyTimelineNcnnBody(argv[2], argv[3], argv[4], argv[5]);
        return 0;
      }
      std::cerr << "Usage: --timeline-ncnn-preview PROJECT MODEL_DIRECTORY [SECONDS]\n"
                   "       --timeline-ncnn-export PROJECT MODEL_DIRECTORY OUTPUT\n"
                   "       --timeline-verify-ncnn SOURCE MODEL_DIRECTORY DIRECTORY\n"
                   "       --timeline-verify-ncnn-mask SOURCE MODEL_DIRECTORY DIRECTORY [--mixed-models]\n"
                   "       --timeline-verify-ncnn-body SOURCE MODEL_DIRECTORY LANDMARKS DIRECTORY\n";
#endif
      std::cerr << "Usage: --timeline-preview PROJECT [SECONDS]\n"
                   "       --timeline-export PROJECT OUTPUT [--software-only]\n"
                   "       --timeline-verify FIRST SECOND DIRECTORY\n"
                   "       --timeline-verify-codecs TEMPLATE_PROJECT DIRECTORY\n"
                   "       --timeline-verify-transition FIRST SECOND DIRECTORY\n"
                   "       --timeline-assets PROJECT REPLACEMENT\n"
                   "       --timeline-verify-assets FIRST REPLACEMENT DIRECTORY\n"
                   "       --timeline-verify-tracking DIRECTORY\n"
                   "       --timeline-verify-masks DIRECTORY\n"
                   "       --timeline-verify-depth PROJECT DIRECTORY\n"
                   "       --timeline-verify-template-masks PROJECT DIRECTORY\n"
                   "       --timeline-verify-animated-template-masks PROJECT DIRECTORY\n"
                   "       --timeline-mask-preview PROJECT [SECONDS]\n"
                   "       --timeline-tracking-preview PROJECT [SECONDS]\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
    }
    return 1;
  }
  if (argc > 1 && std::string_view(argv[1]).starts_with("--media-")) {
    try {
      const std::string_view command(argv[1]);
      if (command == "--media-preview" && (argc == 3 || argc == 4)) {
        avkit_preview::ConfigureFilePreview(argv[2], argc == 4 ? argv[3] : "0");
        const int result = huxerui::RunApplication();
        avkit_preview::StopFilePreview();
        return result;
      }
      if (command == "--media-export" && (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--software-only"))) {
        avkit_preview::ExportFilePreview(argv[2], argv[3], argc == 5);
        return 0;
      }
      if (command == "--media-verify" && argc == 4) {
        avkit_preview::VerifyFilePreview(argv[2], argv[3]);
        return 0;
      }
      std::cerr << "Usage: --media-preview INPUT [SECONDS]\n"
                   "       --media-export INPUT OUTPUT [--software-only]\n"
                   "       --media-verify INPUT OUTPUT\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
    }
    return 1;
  }
#endif
  return huxerui::RunApplication();
}
