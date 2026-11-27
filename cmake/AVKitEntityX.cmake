include(FetchContent)

if (NOT TARGET AVKit::EntityX)
    find_package(Git REQUIRED)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()

    # Upstream uses process-wide family IDs. Independent AVKit sequences can register new types concurrently.
    # The patch makes their allocation atomic and replaces the removed std::iterator base.
    FetchContent_Declare(avkit_entityx
            URL "https://codeload.github.com/alecthomas/entityx/tar.gz/refs/tags/1.3.0"
            URL_HASH SHA256=2cd56d4fc5c553b786b8caf0b5bd9231434f21d43ca0e963d3bc5ee503a06222
            PATCH_COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=<SOURCE_DIR>/.."
                "${GIT_EXECUTABLE}" apply "${CMAKE_CURRENT_LIST_DIR}/entityx-1.3.0.patch"
            SOURCE_SUBDIR avkit
    )
    FetchContent_MakeAvailable(avkit_entityx)

    set(ENTITYX_MAX_COMPONENTS 64)
    set(ENTITYX_DT_TYPE double)
    configure_file("${avkit_entityx_SOURCE_DIR}/entityx/config.h.in"
                   "${avkit_entityx_BINARY_DIR}/entityx/config.h" @ONLY)

    add_library(avkit_entityx STATIC
            "${avkit_entityx_SOURCE_DIR}/entityx/Entity.cc"
            "${avkit_entityx_SOURCE_DIR}/entityx/Event.cc"
            "${avkit_entityx_SOURCE_DIR}/entityx/System.cc"
            "${avkit_entityx_SOURCE_DIR}/entityx/help/Pool.cc"
            "${avkit_entityx_SOURCE_DIR}/entityx/help/Timer.cc"
    )
    add_library(AVKit::EntityX ALIAS avkit_entityx)
    target_include_directories(avkit_entityx SYSTEM PUBLIC "${avkit_entityx_SOURCE_DIR}" "${avkit_entityx_BINARY_DIR}")
    target_compile_features(avkit_entityx PUBLIC cxx_std_20)
    set_target_properties(avkit_entityx PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
endif ()
