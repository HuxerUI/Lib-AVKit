include(FetchContent)

function(avkit_fetch_ncnn)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()

    FetchContent_Declare(avkit_ncnn
            URL "https://codeload.github.com/Tencent/ncnn/tar.gz/refs/tags/20261020"
            URL_HASH SHA256=968d8d8e65ccadc803322f8238736176e0f9c929fcf484895f58a63e6b227127
    )
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(NCNN_VERSION 20261020)
    set(NCNN_SHARED_LIB OFF)
    set(NCNN_OPENMP OFF)
    set(NCNN_VULKAN OFF)
    set(NCNN_PLATFORM_API OFF)
    set(NCNN_STDIO OFF)
    set(NCNN_STRING ON)
    # ncnn otherwise exports flags that disable exception handling and RTTI to mobile consumers.
    set(NCNN_DISABLE_EXCEPTION OFF)
    set(NCNN_DISABLE_RTTI OFF)
    set(NCNN_INSTALL_SDK OFF)
    set(NCNN_BUILD_TESTS OFF)
    set(NCNN_BUILD_TOOLS OFF)
    set(NCNN_BUILD_EXAMPLES OFF)
    set(NCNN_BUILD_BENCHMARK OFF)
    if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        set(NCNN_THREADS OFF)
    endif ()
    FetchContent_MakeAvailable(avkit_ncnn)
endfunction()

if (NOT TARGET ncnn)
    avkit_fetch_ncnn()
endif ()

add_library(avkit_ncnn STATIC
        src/analysis/ncnn_body.cpp
        src/analysis/ncnn_face.cpp
        src/analysis/ncnn_model.cpp
        src/analysis/ncnn_portrait.cpp
        src/analysis/ncnn_provider.cpp
)
add_library(AVKit::NCNN ALIAS avkit_ncnn)
target_link_libraries(avkit_ncnn PUBLIC AVKit::Core PRIVATE ncnn)
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    find_package(Threads REQUIRED)
    target_link_libraries(avkit_ncnn PRIVATE Threads::Threads)
endif ()
set_target_properties(avkit_ncnn PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
