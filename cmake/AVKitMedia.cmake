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

function(avkit_find_ffmpeg)
    if (ANDROID)
        set(AVKIT_ANDROID_FFMPEG_ROOT "$ENV{AVKIT_ANDROID_FFMPEG_ROOT}" CACHE PATH "Android FFmpeg installation prefixes by ABI")
        if (AVKIT_ANDROID_FFMPEG_ROOT)
            set(avkit_ffmpeg_prefix "${AVKIT_ANDROID_FFMPEG_ROOT}/${ANDROID_ABI}")
            foreach (library IN ITEMS avcodec avutil swresample)
                if (NOT EXISTS "${avkit_ffmpeg_prefix}/lib/lib${library}.a"
                        OR NOT EXISTS "${avkit_ffmpeg_prefix}/lib/pkgconfig/lib${library}.pc")
                    message(FATAL_ERROR "Set AVKIT_ANDROID_FFMPEG_ROOT to a directory containing the static FFmpeg installation for ${ANDROID_ABI}")
                endif ()
            endforeach ()
            list(PREPEND CMAKE_PREFIX_PATH "${avkit_ffmpeg_prefix}")
            list(PREPEND CMAKE_FIND_ROOT_PATH "${avkit_ffmpeg_prefix}")
        endif ()
    endif ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AVKIT_FFMPEG REQUIRED IMPORTED_TARGET
            "libavcodec>=60" "libavutil>=58" libswresample
    )
endfunction()

avkit_find_ffmpeg()
add_library(avkit_ffmpeg STATIC src/media/codec/ffmpeg_codec.cpp)
add_library(AVKit::FFmpeg ALIAS avkit_ffmpeg)
target_include_directories(avkit_ffmpeg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(avkit_ffmpeg PUBLIC AVKit::Media PRIVATE PkgConfig::AVKIT_FFMPEG)
set_target_properties(avkit_ffmpeg PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
