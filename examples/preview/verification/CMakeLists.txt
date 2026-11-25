huxerui_add_app(avkit_verify
        SOURCES
            main.cpp
            capture_verify.cpp
            camera_recording_verify.cpp
            file_media_verify.mm
            metal_verify.mm
            movie_renderer.mm
            sticker_scene.cpp
            timeline_depth_verify.mm
            timeline_mask_verify.mm
            timeline_ncnn_verify.mm
            timeline_pag_verify.mm
            timeline_playback_verify.mm
            timeline_strip_verify.mm
            timeline_template_mask_verify.mm
            timeline_tracking_verify.mm
            timeline_verify.mm
            tracking_fixture.cpp
            ../src/timeline_playback.cpp
            ../src/timeline_resources.cpp
            ../src/timeline_strip.cpp
            ../src/workbench_overlay.cpp
            ../src/workbench_photo.cpp
            ../src/workbench_player.cpp
            ../src/workbench_project.cpp
            ../platform/macos/src/workbench_platform.mm
        RESOURCES
            ../resources
        RESOURCE_NAMESPACE
            app
        BUNDLE_NAME
            "AVKit Verification"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.avkit.verify"
)

huxerui_use_library(avkit_verify TARGET HuxerUI::AVKit)
huxerui_use_library(avkit_verify TARGET HuxerUI::Camera)
target_link_libraries(avkit_verify PRIVATE AVKit::Capture bimg_decode bimg "-framework ImageIO")
target_include_directories(avkit_verify PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        ../src
        ../../../src
        ../../../platform/macos/src
)
target_compile_options(avkit_verify PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
set_target_properties(avkit_verify PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF
        MACOSX_BUNDLE_INFO_PLIST "${HUXERUI_MACOS_INFO_PLIST}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)
avkit_embed_macos_pag(avkit_verify)
