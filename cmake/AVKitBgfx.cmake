include(FetchContent)

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
    set(BGFX_BUILD_TOOLS ON)
    set(BGFX_BUILD_TOOLS_SHADER ON)
    set(BGFX_BUILD_TOOLS_BIN2C OFF)
    set(BGFX_BUILD_TOOLS_GEOMETRY OFF)
    set(BGFX_BUILD_TOOLS_TEXTURE OFF)
    set(BGFX_INSTALL OFF)
    set(BGFX_CONFIG_VIDEO OFF)
    set(BGFX_CONFIG_MULTITHREADED OFF)
    FetchContent_MakeAvailable(avkit_bgfx_cmake)
endfunction()

avkit_fetch_bgfx()
FetchContent_GetProperties(bgfx SOURCE_DIR avkit_bgfx_source)

set(avkit_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/render/bgfx/shaders")
set(avkit_shader_output "${CMAKE_CURRENT_BINARY_DIR}/avkit-shaders")
file(MAKE_DIRECTORY "${avkit_shader_output}")

function(avkit_compile_shader name stage platform profile)
    add_custom_command(
            OUTPUT "${avkit_shader_output}/${name}.h"
            COMMAND "$<TARGET_FILE:bgfx::shaderc>"
                    -f "${avkit_shader_dir}/${name}.sc"
                    -o "${avkit_shader_output}/${name}.h"
                    --bin2c "${name}" --platform "${platform}" --type "${stage}" -p "${profile}"
                    --varyingdef "${avkit_shader_dir}/varying.def.sc"
                    -i "${avkit_bgfx_source}/src"
            DEPENDS bgfx::shaderc
                    "${avkit_shader_dir}/${name}.sc"
                    "${avkit_shader_dir}/varying.def.sc"
                    "${avkit_bgfx_source}/src/bgfx_shader.sh"
            VERBATIM
    )
endfunction()
