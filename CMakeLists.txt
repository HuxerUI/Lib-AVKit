cmake_minimum_required(VERSION 3.21)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(av_kit VERSION 0.1.0 LANGUAGES NONE)

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

set(AVKIT_LIB_MEDIA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-Media" CACHE PATH "Lib-Media source checkout")
if (TARGET HuxerUI::Media)
    huxerui_use_library(huxerui_avkit TARGET HuxerUI::Media)
else ()
    huxerui_use_library(huxerui_avkit TARGET HuxerUI::Media PATH "${AVKIT_LIB_MEDIA_SOURCE_DIR}")
endif ()

option(AVKIT_ENABLE_FILE_MEDIA "Build the file media module and Lib-Media adapter" OFF)
if (AVKIT_ENABLE_FILE_MEDIA)
    option(AVKIT_ENABLE_FFMPEG "Build the FFmpeg software codec provider" ON)
endif ()

option(AVKIT_ENABLE_NCNN "Build the ncnn CPU vision analysis provider" OFF)
option(AVKIT_ENABLE_PAG "Build the libpag animation adapter" OFF)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_avkit PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

include(cmake/AVKitFlecs.cmake)

add_library(avkit_core STATIC
        src/analysis/tracking.cpp
        src/ecs/scene.cpp
        src/project/edit_template.cpp
        src/project/project.cpp
        src/project/project_io.cpp
        src/render/image_surface.cpp
)
add_library(AVKit::Core ALIAS avkit_core)
target_compile_features(avkit_core PUBLIC cxx_std_20)
target_include_directories(avkit_core
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(avkit_core PUBLIC flecs::flecs_static HuxerUI::Media)
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
target_link_libraries(huxerui_avkit PUBLIC AVKit::Bgfx)

if (AVKIT_ENABLE_FILE_MEDIA)
    include(cmake/AVKitMedia.cmake)
endif ()

if (AVKIT_ENABLE_NCNN)
    include(cmake/AVKitNCNN.cmake)
endif ()

if (AVKIT_ENABLE_PAG)
    include(cmake/AVKitPAG.cmake)
endif ()
