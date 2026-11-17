avkit_compile_shader(avkit_vs_composite vertex osx metal)
avkit_compile_shader(avkit_fs_composite fragment osx metal)
avkit_compile_shader(avkit_fs_masked_blend fragment osx metal)

add_library(avkit_bgfx STATIC
        platform/macos/src/bgfx_backend_macos.mm
        "${avkit_shader_output}/avkit_vs_composite.h"
        "${avkit_shader_output}/avkit_fs_composite.h"
        "${avkit_shader_output}/avkit_fs_masked_blend.h"
)
add_library(AVKit::Bgfx ALIAS avkit_bgfx)
set_target_properties(avkit_bgfx PROPERTIES
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED ON
        OBJCXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
)
target_compile_options(avkit_bgfx PRIVATE -fobjc-arc)
target_include_directories(avkit_bgfx PRIVATE "${avkit_shader_output}")
target_link_libraries(avkit_bgfx PUBLIC AVKit::Core PRIVATE bgfx "-framework Metal")
