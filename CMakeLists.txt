cmake_minimum_required(VERSION 4.2)
project(codotaku_web)

set(CMAKE_CXX_STANDARD 26)

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM source cache directory" FORCE)

set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/CPM.cmake")
if (NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
    message(STATUS "Downloading CPM.cmake...")
    file(
            DOWNLOAD
            https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.8/CPM.cmake
            "${CPM_DOWNLOAD_LOCATION}"
            EXPECTED_HASH SHA256=78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791
    )
else ()
    message(STATUS "Using cached CPM.cmake")
endif ()
include("${CPM_DOWNLOAD_LOCATION}")

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
    message(STATUS "Skia sources cached in ${skia_SOURCE_DIR}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(SKIA_GSD_LOCK "${skia_SOURCE_DIR}/.git-sync-deps-done")
    if (NOT EXISTS "${SKIA_GSD_LOCK}")
        message(STATUS "git-sync-deps: fetching Skia third-party dependencies...")
        set(SKIA_EXTERNALS_DIR "${skia_SOURCE_DIR}/third_party/externals")
        if (EXISTS "${SKIA_EXTERNALS_DIR}")
            file(REMOVE_RECURSE "${SKIA_EXTERNALS_DIR}")
        endif ()

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
        file(TOUCH "${SKIA_GSD_LOCK}")
        message(STATUS "git-sync-deps complete")
    else ()
        message(STATUS "git-sync-deps: already done")
    endif ()

    # Build Skia for Debug and Release at configure time.
    foreach (SKIA_CFG Debug Release)
        if (MSVC)
            if (SKIA_CFG STREQUAL "Debug")
                set(SKIA_GN_ARGS "is_official_build=false is_debug=true extra_cflags=[\"/MDd\"]")
            else ()
                set(SKIA_GN_ARGS "is_official_build=false is_debug=false extra_cflags=[\"/MD\"]")
            endif ()
        else ()
            if (SKIA_CFG STREQUAL "Debug")
                set(SKIA_GN_ARGS "is_official_build=false is_debug=true")
            else ()
                set(SKIA_GN_ARGS "is_official_build=false is_debug=false")
            endif ()
        endif ()
        set(SKIA_GN_ARGS "${SKIA_GN_ARGS} skia_use_gl=false skia_enable_ganesh=false skia_enable_graphite=true skia_use_vulkan=true skia_use_freetype=true skia_use_system_freetype2=false skia_enable_pdf=false skia_enable_skottie=false skia_enable_svg=false skia_use_xps=false skia_use_dng_sdk=false skia_use_piex=false")

        set(SKIA_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/skia-build/${SKIA_CFG}/out/Release")
        set(SKIA_LIB "${SKIA_OUT_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}skia${CMAKE_STATIC_LIBRARY_SUFFIX}")

        if (NOT EXISTS "${SKIA_LIB}")
            message(STATUS "Building Skia (${SKIA_CFG})...")
            execute_process(
                COMMAND "${skia_SOURCE_DIR}/bin/gn" gen "${SKIA_OUT_DIR}"
                    "--args=${SKIA_GN_ARGS}"
                    "--root=${skia_SOURCE_DIR}"
                WORKING_DIRECTORY "${skia_SOURCE_DIR}"
                RESULT_VARIABLE GN_RESULT
                OUTPUT_VARIABLE GN_OUT
                ERROR_VARIABLE GN_OUT
            )
            if (NOT GN_RESULT EQUAL 0)
                message(FATAL_ERROR "gn gen (${SKIA_CFG}) failed: ${GN_OUT}")
            endif ()
            execute_process(
                COMMAND ninja -C "${SKIA_OUT_DIR}" skia
                RESULT_VARIABLE NINJA_RESULT
                OUTPUT_VARIABLE NINJA_OUT
                ERROR_VARIABLE NINJA_OUT
            )
            if (NOT NINJA_RESULT EQUAL 0)
                message(FATAL_ERROR "ninja (${SKIA_CFG}) failed: ${NINJA_OUT}")
            endif ()
        else ()
            message(STATUS "Skipping Skia ${SKIA_CFG} (already built)")
        endif ()
    endforeach ()

    add_library(Skia::Skia STATIC IMPORTED GLOBAL)
    target_include_directories(Skia::Skia INTERFACE "${skia_SOURCE_DIR}")
    set_target_properties(Skia::Skia PROPERTIES
        IMPORTED_LOCATION_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/skia-build/Debug/out/Release/${CMAKE_STATIC_LIBRARY_PREFIX}skia${CMAKE_STATIC_LIBRARY_SUFFIX}"
        IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/skia-build/Release/out/Release/${CMAKE_STATIC_LIBRARY_PREFIX}skia${CMAKE_STATIC_LIBRARY_SUFFIX}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${CMAKE_CURRENT_BINARY_DIR}/skia-build/Release/out/Release/${CMAKE_STATIC_LIBRARY_PREFIX}skia${CMAKE_STATIC_LIBRARY_SUFFIX}"
        IMPORTED_LOCATION_MINSIZEREL "${CMAKE_CURRENT_BINARY_DIR}/skia-build/Release/out/Release/${CMAKE_STATIC_LIBRARY_PREFIX}skia${CMAKE_STATIC_LIBRARY_SUFFIX}"
        IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
    )
endif ()

add_executable(codotaku_web main.cpp)
target_link_libraries(codotaku_web PRIVATE Vulkan::Headers SDL3::SDL3-static vk-bootstrap::vk-bootstrap Skia::Skia)
