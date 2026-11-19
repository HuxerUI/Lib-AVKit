include(FetchContent)

set(AVKIT_BGFX_SHADERC "" CACHE FILEPATH "Host bgfx shader compiler from the pinned bgfx revision")

if (APPLE)
    enable_language(OBJCXX)
endif ()

function(avkit_fetch_bgfx)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()

    # Keep the source and shader compiler on the revisions selected by this bgfx.cmake commit.
    set(avkit_bgfx_owner "https://codeload.github.com/bkaradzic")
    FetchContent_Declare(bx
            URL "${avkit_bgfx_owner}/bx/tar.gz/f8508dc026d5bdde356f767fdfccb8d7253140c1"
            URL_HASH SHA256=2c7022b5fd7b717c6f8913d041b0c7458c11a1f7a8abcbd9b420aa55aee1bb44
    )
    FetchContent_Declare(bimg
            URL "${avkit_bgfx_owner}/bimg/tar.gz/9bdb5805ea0f17ee45d2872282ab319a4f59cde3"
            URL_HASH SHA256=2d9170dccb703894f8fee257dd699283c09614bcf6597860526d931297ceeb72
    )
    FetchContent_Declare(bgfx
            URL "${avkit_bgfx_owner}/bgfx/tar.gz/0762725e7dbf2863d68ed92bf26348a0b117c79f"
            URL_HASH SHA256=5a706ddce60c8814e7d98e12e8c4602bb59f61d4d0616ee9fd70af5cd9c5e9db
    )
    FetchContent_Declare(avkit_bgfx_cmake
            URL "${avkit_bgfx_owner}/bgfx.cmake/tar.gz/82f885c710b98a4b4f4df40cc234a3cb702f4846"
            URL_HASH SHA256=51c46f89f71a567fe345f698ad53e4e8b38bd1b809fb67ebe3761965135c1cad
    )

    set(BGFX_BUILD_EXAMPLES OFF)
    if (CMAKE_CROSSCOMPILING OR AVKIT_BGFX_SHADERC)
        set(BGFX_BUILD_TOOLS OFF)
        set(BGFX_BUILD_TOOLS_SHADER OFF)
    else ()
        set(BGFX_BUILD_TOOLS ON)
        set(BGFX_BUILD_TOOLS_SHADER ON)
    endif ()
    set(BGFX_BUILD_TOOLS_BIN2C OFF)
    set(BGFX_BUILD_TOOLS_GEOMETRY OFF)
    set(BGFX_BUILD_TOOLS_TEXTURE OFF)
    set(BGFX_INSTALL OFF)
    # Linux uses an existing GDK context for OpenGL texture transfer.
    set(BGFX_WITH_WAYLAND OFF)
    set(BGFX_CONFIG_VIDEO OFF)
    set(BGFX_CONFIG_MULTITHREADED OFF)
    FetchContent_MakeAvailable(avkit_bgfx_cmake)
endfunction()

avkit_fetch_bgfx()
FetchContent_GetProperties(bgfx SOURCE_DIR avkit_bgfx_source)

set(avkit_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/render/bgfx/shaders")
set(avkit_shader_output "${CMAKE_CURRENT_BINARY_DIR}/avkit-shaders")
file(MAKE_DIRECTORY "${avkit_shader_output}")

if (AVKIT_BGFX_SHADERC)
    set(avkit_shaderc "${AVKIT_BGFX_SHADERC}")
    set(avkit_shaderc_dep "${AVKIT_BGFX_SHADERC}")
elseif (CMAKE_CROSSCOMPILING)
    include(ExternalProject)
    FetchContent_GetProperties(avkit_bgfx_cmake SOURCE_DIR avkit_bgfx_cmake_source)
    FetchContent_GetProperties(bx SOURCE_DIR avkit_bx_source)
    FetchContent_GetProperties(bimg SOURCE_DIR avkit_bimg_source)

    set(avkit_shaderc_bin "${CMAKE_CURRENT_BINARY_DIR}/avkit-shaderc-host/bin")
    set(avkit_shaderc "${avkit_shaderc_bin}/shaderc")
    if (CMAKE_HOST_WIN32)
        string(APPEND avkit_shaderc ".exe")
    endif ()

    # Use the same pinned sources with the host compiler and SDK.
    set(avkit_host_cmake "${CMAKE_COMMAND}" -E env
            --unset=CMAKE_TOOLCHAIN_FILE --unset=CC --unset=CXX --unset=CFLAGS --unset=CXXFLAGS
            --unset=CPPFLAGS --unset=LDFLAGS --unset=SDKROOT --unset=MACOSX_DEPLOYMENT_TARGET
            --unset=CMAKE_GENERATOR_PLATFORM --unset=CMAKE_GENERATOR_TOOLSET
            "${CMAKE_COMMAND}"
    )
    ExternalProject_Add(avkit_bgfx_host_shaderc
            SOURCE_DIR "${avkit_bgfx_cmake_source}"
            PREFIX "${CMAKE_CURRENT_BINARY_DIR}/avkit-shaderc-host"
            EXCLUDE_FROM_ALL TRUE
            CONFIGURE_COMMAND ${avkit_host_cmake}
                    -S <SOURCE_DIR> -B <BINARY_DIR> -G "${CMAKE_GENERATOR}"
                    "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
                    -DCMAKE_BUILD_TYPE=Release
                    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE:PATH=${avkit_shaderc_bin}"
                    "-DBX_DIR:PATH=${avkit_bx_source}"
                    "-DBIMG_DIR:PATH=${avkit_bimg_source}"
                    "-DBGFX_DIR:PATH=${avkit_bgfx_source}"
                    -DBGFX_BUILD_EXAMPLES=OFF -DBGFX_BUILD_TOOLS=ON -DBGFX_BUILD_TOOLS_SHADER=ON
                    -DBGFX_BUILD_TOOLS_BIN2C=OFF -DBGFX_BUILD_TOOLS_GEOMETRY=OFF
                    -DBGFX_BUILD_TOOLS_TEXTURE=OFF -DBGFX_INSTALL=OFF -DBGFX_WITH_WAYLAND=OFF
                    -DBGFX_CONFIG_VIDEO=OFF
            BUILD_COMMAND ${avkit_host_cmake} --build <BINARY_DIR> --config Release --target shaderc
            BUILD_ALWAYS TRUE
            BUILD_BYPRODUCTS "${avkit_shaderc}"
            INSTALL_COMMAND ""
    )
    set(avkit_shaderc_dep avkit_bgfx_host_shaderc "${avkit_shaderc}")
else ()
    set(avkit_shaderc "$<TARGET_FILE:bgfx::shaderc>")
    set(avkit_shaderc_dep bgfx::shaderc)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(avkit_shader_platform osx)
    set(avkit_shader_profile metal)
    set(avkit_renderer Metal)
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(avkit_shader_platform ios)
    set(avkit_shader_profile metal)
    set(avkit_renderer Metal)
elseif (ANDROID OR EMSCRIPTEN)
    if (ANDROID)
        set(avkit_shader_platform android)
    else ()
        set(avkit_shader_platform asm.js)
    endif ()
    set(avkit_shader_profile 300_es)
    set(avkit_renderer OpenGLES)
elseif (WIN32 AND CMAKE_HOST_WIN32)
    set(avkit_shader_platform windows)
    set(avkit_shader_profile s_5_0)
    set(avkit_renderer Direct3D11)
elseif (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (WIN32)
        set(avkit_shader_platform windows)
    else ()
        set(avkit_shader_platform linux)
    endif ()
    set(avkit_shader_profile spirv)
    set(avkit_renderer Vulkan)
else ()
    message(FATAL_ERROR "No bgfx renderer configured for ${CMAKE_SYSTEM_NAME}")
endif ()

function(avkit_compile_shader name stage profile suffix)
    set(output "${avkit_shader_output}/${name}${suffix}.h")
    add_custom_command(
            OUTPUT "${output}"
            COMMAND "${avkit_shaderc}"
                    -f "${avkit_shader_dir}/${name}.sc"
                    -o "${output}"
                    --bin2c "${name}${suffix}" --platform "${avkit_shader_platform}" --type "${stage}" -p "${profile}"
                    --varyingdef "${avkit_shader_dir}/varying.def.sc"
                    -i "${avkit_bgfx_source}/src"
            DEPENDS ${avkit_shaderc_dep}
                    "${avkit_shader_dir}/${name}.sc"
                    "${avkit_shader_dir}/varying.def.sc"
                    "${avkit_shader_dir}/avkit_alpha.sh"
                    "${avkit_bgfx_source}/src/bgfx_shader.sh"
            VERBATIM
    )
    target_sources(avkit_bgfx PRIVATE "${output}")
endfunction()

add_library(avkit_bgfx STATIC
        src/render/bgfx/bgfx_backend.cpp
        src/render/bgfx/video_texture.cpp
)
add_library(AVKit::Bgfx ALIAS avkit_bgfx)
target_compile_features(avkit_bgfx PRIVATE cxx_std_20)
set_target_properties(avkit_bgfx PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(avkit_bgfx PRIVATE "AVKIT_BGFX_RENDERER=${avkit_renderer}")
target_include_directories(avkit_bgfx PRIVATE "${avkit_shader_output}" src/render/bgfx)
target_link_libraries(avkit_bgfx PUBLIC AVKit::Core PRIVATE bgfx)
if (ANDROID)
    target_sources(avkit_bgfx PRIVATE platform/android/src/bgfx_gles_texture.cpp)
    target_link_libraries(avkit_bgfx PRIVATE android EGL GLESv3)
elseif (EMSCRIPTEN)
    target_sources(avkit_bgfx PRIVATE platform/web/src/bgfx_webgl_texture.cpp)
    target_compile_options(avkit_bgfx PRIVATE -fexceptions)
    target_link_options(avkit_bgfx INTERFACE "-sMIN_WEBGL_VERSION=2")
elseif (APPLE)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_sources(avkit_bgfx PRIVATE platform/ios/src/bgfx_metal_texture.mm)
    else ()
        target_sources(avkit_bgfx PRIVATE platform/macos/src/bgfx_metal_texture.mm)
    endif ()
    set_target_properties(avkit_bgfx PROPERTIES
            OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    target_compile_options(avkit_bgfx PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(avkit_bgfx PRIVATE "-framework Metal" "-framework CoreVideo" "-framework CoreImage")
elseif (avkit_renderer STREQUAL "Direct3D11")
    target_sources(avkit_bgfx PRIVATE platform/windows/src/bgfx_d3d11_texture.cpp)
    target_compile_definitions(bgfx PRIVATE BGFX_CONFIG_RENDERER_DIRECT3D11=1)
    target_link_libraries(avkit_bgfx PRIVATE d3d11)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AVKIT_GDK_GL REQUIRED IMPORTED_TARGET "gtk4>=4.14" epoxy egl)
    target_sources(avkit_bgfx PRIVATE platform/linux/src/bgfx_gl_texture.cpp)
    target_compile_definitions(bgfx PRIVATE BGFX_CONFIG_RENDERER_VULKAN=1 BGFX_CONFIG_RENDERER_OPENGL=43)
    target_link_libraries(avkit_bgfx PRIVATE PkgConfig::AVKIT_GDK_GL)
    avkit_compile_shader(avkit_vs_composite vertex 430 _gl)
    avkit_compile_shader(avkit_fs_composite fragment 430 _gl)
    avkit_compile_shader(avkit_fs_masked_blend fragment 430 _gl)
elseif (avkit_renderer STREQUAL "Vulkan")
    target_sources(avkit_bgfx PRIVATE platform/windows/src/bgfx_vulkan_backend.cpp)
    target_compile_definitions(bgfx PRIVATE BGFX_CONFIG_RENDERER_VULKAN=1)
endif ()

avkit_compile_shader(avkit_vs_composite vertex "${avkit_shader_profile}" "")
avkit_compile_shader(avkit_fs_composite fragment "${avkit_shader_profile}" "")
avkit_compile_shader(avkit_fs_masked_blend fragment "${avkit_shader_profile}" "")
