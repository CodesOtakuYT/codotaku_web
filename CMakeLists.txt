cmake_minimum_required(VERSION 4.2)
project(codotaku_web)

set(CMAKE_CXX_STANDARD 26)

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM source cache directory" FORCE)

file(
        DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.8/CPM.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
        EXPECTED_HASH SHA256=78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

CPMAddPackage(
        NAME SDL
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.10
        OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON"
)
CPMAddPackage("gh:KhronosGroup/Vulkan-Headers#vulkan-sdk-1.4.350.1")
CPMAddPackage("gh:charles-lunarg/vk-bootstrap#v1.4.354")

CPMAddPackage(
        NAME skia
        GIT_REPOSITORY https://github.com/google/skia.git
        GIT_TAG 49856493d135b293731a00bf5f5a9d5969b86451
        DOWNLOAD_ONLY YES
)

if (skia_ADDED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Clean any stale externals to avoid git checkout conflicts on re-sync
    set(SKIA_EXTERNALS_DIR "${skia_SOURCE_DIR}/third_party/externals")
    if (EXISTS "${SKIA_EXTERNALS_DIR}")
        file(REMOVE_RECURSE "${SKIA_EXTERNALS_DIR}")
    endif ()

    # git-sync-deps is inherently unreliable with multithreaded cloning
    # hitting HTTP 429 rate limits from googlesource.com; retry up to 3 times.
    set(SKIA_SYNC_DEPS_RETRIES 3)
    set(SKIA_SYNC_SUCCESS FALSE)
    foreach (SKIA_TRY RANGE ${SKIA_SYNC_DEPS_RETRIES})
        execute_process(
                COMMAND ${Python3_EXECUTABLE} tools/git-sync-deps
                WORKING_DIRECTORY "${skia_SOURCE_DIR}"
                RESULT_VARIABLE SKIA_GSD_RESULT
        )
        if (SKIA_GSD_RESULT EQUAL 0)
            set(SKIA_SYNC_SUCCESS TRUE)
            break()
        endif ()
        message(STATUS "git-sync-deps failed (attempt ${SKIA_TRY}), retrying...")
    endforeach ()
    if (NOT SKIA_SYNC_SUCCESS)
        message(FATAL_ERROR "git-sync-deps failed after ${SKIA_SYNC_DEPS_RETRIES} attempts")
    endif ()

    set(SKIA_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/skia-build")
    set(SKIA_GN_OUTPUT_DIR "${SKIA_BINARY_DIR}/out/Release")
    set(SKIA_LIB_OUTPUT "${SKIA_GN_OUTPUT_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}skia${CMAKE_STATIC_LIBRARY_SUFFIX}")

    include(ExternalProject)

    # Match Skia's runtime library to CMake's build type
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(SKIA_GN_EXTRA_ARGS "is_debug=true extra_cflags=[\"/MDd\"]")
    else ()
        set(SKIA_GN_EXTRA_ARGS "is_debug=false extra_cflags=[\"/MD\"]")
    endif ()

    ExternalProject_Add(skia_external
            SOURCE_DIR ${skia_SOURCE_DIR}
            BINARY_DIR ${SKIA_BINARY_DIR}
            DOWNLOAD_COMMAND ""
            UPDATE_COMMAND ""
            INSTALL_COMMAND ""
            CONFIGURE_COMMAND ${skia_SOURCE_DIR}/bin/gn gen ${SKIA_GN_OUTPUT_DIR} "--args=is_official_build=false ${SKIA_GN_EXTRA_ARGS} skia_use_freetype=false skia_enable_graphite=true skia_use_vulkan=true" --root=${skia_SOURCE_DIR}
            BUILD_COMMAND ninja -C ${SKIA_GN_OUTPUT_DIR} skia
            BYPRODUCTS ${SKIA_LIB_OUTPUT}
            USES_TERMINAL_CONFIGURE on
            USES_TERMINAL_BUILD on
    )

    add_library(Skia::Skia STATIC IMPORTED GLOBAL)
    add_dependencies(Skia::Skia skia_external)
    target_include_directories(Skia::Skia
            INTERFACE
            "${skia_SOURCE_DIR}"
    )
    set_property(TARGET Skia::Skia PROPERTY IMPORTED_LOCATION "${SKIA_LIB_OUTPUT}")
endif ()

add_executable(codotaku_web main.cpp)
target_link_libraries(codotaku_web PRIVATE Vulkan::Headers SDL3::SDL3-static vk-bootstrap::vk-bootstrap Skia::Skia)
