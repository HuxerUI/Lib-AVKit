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

set(APP_SOURCE_FILES
        src/app.cpp
        src/workbench.cpp
        src/workbench_editor.cpp
        src/workbench_project.cpp
)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
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

if (APPLE OR ANDROID OR WIN32 OR HUXERUI_LIBRARY_GRAPH_ONLY)
    set(AVKIT_LIB_CAMERA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Lib-Camera"
            CACHE PATH "Lib-Camera source checkout")
    huxerui_use_library(example_av_kit TARGET HuxerUI::Camera PATH "${AVKIT_LIB_CAMERA_SOURCE_DIR}")
endif ()

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(example_av_kit PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR ANDROID)
    if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        enable_language(OBJCXX)
    endif ()
    avkit_add_capture_target()
    target_link_libraries(example_av_kit PRIVATE AVKit::Capture)
    target_sources(example_av_kit PRIVATE src/capture_camera.cpp src/capture_avkit_device.cpp)
elseif (IOS OR WIN32)
    target_sources(example_av_kit PRIVATE src/capture_camera.cpp src/capture_camera_device.cpp)
else ()
    target_sources(example_av_kit PRIVATE src/capture_unavailable.cpp)
endif ()

if (IOS)
    # Include the existing resource-free static targets in the HuxerUI iOS app archive.
    get_target_property(avkit_core_target AVKit::Core ALIASED_TARGET)
    get_target_property(avkit_flecs_target flecs::flecs_static ALIASED_TARGET)
    set_property(TARGET ${avkit_core_target} ${avkit_flecs_target} PROPERTY HUXERUI_LIBRARY TRUE)
    huxerui_use_library(example_av_kit TARGET AVKit::Core)
    huxerui_use_library(example_av_kit TARGET flecs::flecs_static)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR ANDROID)
    target_sources(example_av_kit PRIVATE
            src/timeline_playback.cpp
            src/timeline_resources.cpp
            src/timeline_strip.cpp
            src/workbench_monitor.cpp
            src/workbench_overlay.cpp
            src/workbench_photo.cpp
            src/workbench_player.cpp
    )
    target_link_libraries(example_av_kit PRIVATE bimg_decode bimg)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(example_av_kit PRIVATE
            platform/macos/src/workbench_input.mm
            platform/macos/src/workbench_platform.mm
    )
    set_target_properties(example_av_kit PROPERTIES
            OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF
    )
    target_compile_options(example_av_kit PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    avkit_embed_macos_pag(example_av_kit)
    add_subdirectory(verification EXCLUDE_FROM_ALL)
elseif (ANDROID)
    target_sources(example_av_kit PRIVATE platform/android/src/workbench_platform.cpp)
    target_link_libraries(example_av_kit PRIVATE log)
else ()
    target_sources(example_av_kit PRIVATE
            src/workbench_media_unavailable.cpp
    )
endif ()
