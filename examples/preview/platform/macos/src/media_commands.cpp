#include <exception>
#include <iostream>
#include <string_view>

#include "workbench_media.h"
#include "file_preview.h"
#include "timeline_preview.h"
#include "timeline_depth_verify.h"
#include "timeline_mask_verify.h"
#include "timeline_strip.h"
#include "timeline_template_mask_verify.h"
#include "timeline_tracking_verify.h"
#include "timeline_pag_verify.h"

namespace avkit_preview {

std::optional<int> RunFileMediaCommand(int argc, const char** argv) {
  if (argc > 1 && std::string_view(argv[1]).starts_with("--timeline-")) {
    try {
      const std::string_view command(argv[1]);
      if ((command == "--timeline-preview" || command == "--timeline-tracking-preview") && (argc == 3 || argc == 4)) {
        ConfigureTimelinePreview(argv[2], argc == 4 ? argv[3] : "0",
            command == "--timeline-tracking-preview" ? CreateMockTrackingProvider() : nullptr);
        const int result = huxerui::RunApplication();
        StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-export" && (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--software-only"))) {
        ExportTimelinePreview(argv[2], argv[3], argc == 5);
        return 0;
      }
      if (command == "--timeline-verify" && argc == 5) {
        VerifyTimelinePreview(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-verify-codecs" && argc == 4) {
        VerifyTimelineCodecs(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-transition" && argc == 5) {
        VerifyTimelineTransition(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-assets" && argc == 4) {
        ConfigureTimelineStrip(argv[2], argv[3]);
        const auto result = huxerui::RunApplication();
        StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-verify-assets" && argc == 5) {
        VerifyTimelineStrip(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-verify-tracking" && argc == 3) {
        VerifyTimelineTracking(argv[2]);
        return 0;
      }
      if (command == "--timeline-verify-masks" && argc == 3) {
        VerifyTimelineMasks(argv[2]);
        return 0;
      }
      if (command == "--timeline-verify-depth" && argc == 4) {
        VerifyTimelineDepth(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-template-masks" && argc == 4) {
        VerifyTimelineTemplateMasks(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-animated-template-masks" && argc == 4) {
        VerifyTimelineAnimatedTemplateMasks(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-stickers" && argc == 4) {
        VerifyTimelinePagStickers(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-stickers")
        std::cerr << "Usage: --timeline-verify-pag-stickers PROJECT DIRECTORY\n";
      if (command == "--timeline-verify-pag-replacements" && argc == 4) {
        VerifyTimelinePagReplacements(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-replacements")
        std::cerr << "Usage: --timeline-verify-pag-replacements PROJECT DIRECTORY\n";
      if (command == "--timeline-verify-pag-transition" && argc == 4) {
        VerifyTimelinePagTransition(argv[2], argv[3]);
        return 0;
      }
      if (command == "--timeline-verify-pag-transition")
        std::cerr << "Usage: --timeline-verify-pag-transition PROJECT DIRECTORY\n";
      if (command == "--timeline-mask-preview" && (argc == 3 || argc == 4)) {
        ConfigureTimelinePreview(argv[2], argc == 4 ? argv[3] : "0",
                                                CreateMockMaskProvider(), true);
        const int result = huxerui::RunApplication();
        StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-verify-ncnn-mask" &&
          (argc == 5 || (argc == 6 && std::string_view(argv[5]) == "--mixed-models"))) {
        VerifyTimelineNcnnMasks(argv[2], argv[3], argv[4], argc == 6);
        return 0;
      }
      if (command == "--timeline-ncnn-export" && argc == 5) {
        ExportTimelinePreview(argv[2], argv[4], true, CreateTimelineNcnnProvider(argv[3]));
        return 0;
      }
      if (command == "--timeline-ncnn-preview" && (argc == 4 || argc == 5)) {
        ConfigureTimelinePreview(argv[2], argc == 5 ? argv[4] : "0",
                                                CreateTimelineNcnnProvider(argv[3]), true);
        const auto result = huxerui::RunApplication();
        StopTimelinePreview();
        return result;
      }
      if (command == "--timeline-verify-ncnn" && argc == 5) {
        VerifyTimelineNcnn(argv[2], argv[3], argv[4]);
        return 0;
      }
      if (command == "--timeline-verify-ncnn-body" && argc == 6) {
        VerifyTimelineNcnnBody(argv[2], argv[3], argv[4], argv[5]);
        return 0;
      }
      std::cerr << "Usage: --timeline-ncnn-preview PROJECT MODEL_DIRECTORY [SECONDS]\n"
                   "       --timeline-ncnn-export PROJECT MODEL_DIRECTORY OUTPUT\n"
                   "       --timeline-verify-ncnn SOURCE MODEL_DIRECTORY DIRECTORY\n"
                   "       --timeline-verify-ncnn-mask SOURCE MODEL_DIRECTORY DIRECTORY [--mixed-models]\n"
                   "       --timeline-verify-ncnn-body SOURCE MODEL_DIRECTORY LANDMARKS DIRECTORY\n";
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
        ConfigureFilePreview(argv[2], argc == 4 ? argv[3] : "0");
        const int result = huxerui::RunApplication();
        StopFilePreview();
        return result;
      }
      if (command == "--media-export" && (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--software-only"))) {
        ExportFilePreview(argv[2], argv[3], argc == 5);
        return 0;
      }
      if (command == "--media-verify" && argc == 4) {
        VerifyFilePreview(argv[2], argv[3]);
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
  return std::nullopt;
}

} // namespace avkit_preview
