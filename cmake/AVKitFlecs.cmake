include(FetchContent)

function(avkit_fetch_flecs)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()

    FetchContent_Declare(avkit_flecs
            URL "https://codeload.github.com/SanderMertens/flecs/tar.gz/refs/tags/v4.2.0"
            URL_HASH SHA256=4107b0a36aa25f80ddcd6f939451ccfe1bacafcccdd2695840a4a8c413d05b40
    )
    set(FLECS_STATIC ON)
    set(FLECS_SHARED OFF)
    set(FLECS_TESTS OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    FetchContent_MakeAvailable(avkit_flecs)
endfunction()

if (NOT TARGET flecs::flecs_static)
    avkit_fetch_flecs()
endif ()
