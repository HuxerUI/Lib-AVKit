cmake_minimum_required(VERSION 3.21)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(av_kit VERSION 1.0.0 LANGUAGES NONE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

huxerui_add_library(huxerui_avkit
        SOURCES
            src/avkit.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_avkit
)

add_library(HuxerUI::AVKit ALIAS huxerui_avkit)

set(AVKIT_LIB_MEDIA_SOURCE_DIR "" CACHE PATH "Optional Lib-Media source checkout")
if (TARGET HuxerUI::Media)
    huxerui_use_library(huxerui_avkit TARGET HuxerUI::Media)
elseif (AVKIT_LIB_MEDIA_SOURCE_DIR)
    huxerui_use_library(huxerui_avkit TARGET HuxerUI::Media PATH "${AVKIT_LIB_MEDIA_SOURCE_DIR}")
else ()
    huxerui_use_library(huxerui_avkit
            TARGET HuxerUI::Media
            URL "https://github.com/HuxerUI/Lib-Media.git"
            COMMIT "0cf9f33125bed200563e07626cbe7ae0eb978f38"
    )
endif ()

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_avkit PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

include(cmake/AVKitEntityX.cmake)

add_library(avkit_core STATIC
        src/analysis/tracking.cpp
        src/ecs/scene.cpp
        src/ecs/system/layer_timing_system.cpp
        src/ecs/system/pag_system.cpp
        src/ecs/system/resource_system.cpp
        src/ecs/system/spatial_system.cpp
        src/project/edit_template.cpp
        src/project/project.cpp
        src/project/project_io.cpp
        src/project/resource_catalog.cpp
        src/media/image_decode.cpp
        src/render/image_pixels.cpp
)
add_library(AVKit::Core ALIAS avkit_core)
target_compile_features(avkit_core PUBLIC cxx_std_20)
target_include_directories(avkit_core
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(avkit_core PUBLIC AVKit::EntityX HuxerUI::Media)
set_target_properties(avkit_core PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if (TARGET HuxerUI::huxerui)
    set(avkit_sdk_target HuxerUI::huxerui)
else ()
    set(avkit_sdk_target HuxerUI::huxerui_static)
endif ()
target_include_directories(avkit_core SYSTEM PUBLIC
        "$<TARGET_PROPERTY:${avkit_sdk_target},INTERFACE_INCLUDE_DIRECTORIES>"
)

include(cmake/AVKitBgfx.cmake)
target_link_libraries(avkit_core PRIVATE bimg_decode bimg)
include(cmake/AVKitMedia.cmake)
include(cmake/AVKitNCNN.cmake)
include(cmake/AVKitPAG.cmake)

add_library(avkit_render STATIC src/render/thumbnail_source.cpp
        src/render/timeline_renderer.cpp src/render/timeline_preview.cpp)
add_library(AVKit::Render ALIAS avkit_render)
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(avkit_render PRIVATE platform/macos/src/render_iteration.mm)
    set_target_properties(avkit_render PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
endif ()
target_include_directories(avkit_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if (ANDROID)
    target_include_directories(avkit_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src")
endif ()
target_link_libraries(avkit_render PUBLIC AVKit::Bgfx AVKit::Media AVKit::PAG)
set_target_properties(avkit_render PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

target_link_libraries(huxerui_avkit PUBLIC AVKit::Render AVKit::FFmpeg AVKit::NCNN)

include(cmake/AVKitCapture.cmake)
