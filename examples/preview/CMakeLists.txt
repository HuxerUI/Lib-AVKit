cmake_minimum_required(VERSION 3.21)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_av_kit VERSION 0.1.0 LANGUAGES NONE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

set(APP_SOURCE_FILES src/app.cpp)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

huxerui_add_app(example_av_kit
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "AVKit Preview"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.avkit.preview"
)

huxerui_configure_project_app(example_av_kit)

huxerui_use_library(example_av_kit
        TARGET HuxerUI::AVKit
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    option(AVKIT_ENABLE_CAPTURE_PREVIEW "Build the macOS camera and microphone example" OFF)
    if (AVKIT_ENABLE_CAPTURE_PREVIEW)
        set(AVKIT_LIB_CAMERA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Lib-Camera"
                CACHE PATH "Lib-Camera source checkout")
        huxerui_use_library(example_av_kit TARGET HuxerUI::Camera PATH "${AVKIT_LIB_CAMERA_SOURCE_DIR}")
        if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
            if (NOT TARGET AVKit::FFmpeg)
                message(FATAL_ERROR "The capture example requires the FFmpeg adapter")
            endif ()
            target_compile_definitions(example_av_kit PRIVATE AVKIT_CAPTURE_PREVIEW=1)
        endif ()
    endif ()
endif ()

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
    target_sources(example_av_kit PRIVATE src/sticker_scene.cpp platform/macos/src/metal_preview.mm)
    set_target_properties(example_av_kit PROPERTIES
            OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF
    )
    target_include_directories(example_av_kit PRIVATE
            src
            platform/macos/src
            "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/macos/src"
    )
    target_compile_definitions(example_av_kit PRIVATE AVKIT_MACOS_PREVIEW=1)
    target_compile_options(example_av_kit PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    if (TARGET AVKit::FFmpeg)
        target_sources(example_av_kit PRIVATE
                src/timeline_strip.cpp
                src/timeline_strip_view.cpp
                src/verification/tracking_fixture.cpp
                platform/macos/src/file_preview.mm
                platform/macos/src/movie_renderer.mm
                platform/macos/src/timeline_preview.mm
                platform/macos/src/timeline_strip_macos.mm
                platform/macos/verification/timeline_strip_verify.mm
                platform/macos/verification/timeline_mask_verify.mm
                platform/macos/verification/timeline_template_mask_verify.mm
                platform/macos/verification/timeline_tracking_verify.mm
                platform/macos/verification/timeline_verify.mm
        )
        target_include_directories(example_av_kit PRIVATE
                platform/macos/verification
                "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
        )
        target_compile_definitions(example_av_kit PRIVATE AVKIT_FILE_MEDIA=1)
        target_link_libraries(example_av_kit PRIVATE AVKit::FFmpeg "-framework CoreImage" "-framework ImageIO")
        if (TARGET AVKit::NCNN)
            target_sources(example_av_kit PRIVATE platform/macos/verification/timeline_ncnn_verify.mm)
            target_compile_definitions(example_av_kit PRIVATE AVKIT_NCNN_PREVIEW=1)
            target_link_libraries(example_av_kit PRIVATE AVKit::NCNN)
        endif ()
        if (AVKIT_ENABLE_CAPTURE_PREVIEW)
            target_sources(example_av_kit PRIVATE src/capture.cpp platform/macos/src/live_capture.mm)
        endif ()
    endif ()
else ()
    target_sources(example_av_kit PRIVATE src/bgfx_preview.cpp)
endif ()
