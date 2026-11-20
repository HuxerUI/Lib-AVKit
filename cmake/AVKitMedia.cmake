add_library(avkit_media STATIC
        src/media/asset_sampling.cpp
        src/media/codec/lib_media_codec.cpp
        src/media/file_movie.cpp
        src/media/timeline.cpp
)
add_library(AVKit::Media ALIAS avkit_media)
target_include_directories(avkit_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(avkit_media PUBLIC AVKit::Core)
set_target_properties(avkit_media PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

if (NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(avkit_media PRIVATE Threads::Threads)
endif ()

if (AVKIT_ENABLE_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AVKIT_FFMPEG REQUIRED IMPORTED_TARGET
            "libavcodec>=60" "libavutil>=58" libswresample
    )
    add_library(avkit_ffmpeg STATIC src/media/codec/ffmpeg_codec.cpp)
    add_library(AVKit::FFmpeg ALIAS avkit_ffmpeg)
    target_link_libraries(avkit_ffmpeg PUBLIC AVKit::Media PRIVATE PkgConfig::AVKIT_FFMPEG)
    set_target_properties(avkit_ffmpeg PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
endif ()
