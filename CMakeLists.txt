cmake_minimum_required(VERSION 3.20)
project(OrtaRecomp VERSION 0.1.0 LANGUAGES C)

# Orta Recomp: a static recompilation of Panzer Dragoon Orta (Xbox).
#
# The executable always builds. When generated code is present in PDO_GEN_DIR
# (tools/recomp.py writes it there from your own default.xbe), it is compiled
# in together with the xboxrecomp kernel layer. Without it you get the
# launcher alone, which is enough to test the game-file setup.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(PDO_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen" CACHE PATH
    "Directory holding the generated C from tools/recomp.py")

# ── SDL3 (window, dialogs, file I/O, later rendering/audio/input) ────────

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL EXCLUDE_FROM_ALL)

# ── Launcher ─────────────────────────────────────────────────────────────

add_executable(OrtaRecomp
    src/main.c
    src/config.c
    src/assets.c
    src/setup_ui.c
    src/xiso.c
    src/xbe.c
    src/sha256.c
)
target_include_directories(OrtaRecomp PRIVATE src)
target_link_libraries(OrtaRecomp PRIVATE SDL3::SDL3-static)

if(MSVC)
    target_compile_options(OrtaRecomp PRIVATE /W4 /wd4100)
    target_compile_definitions(OrtaRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Console app during development, so logs and --check output are visible.
    # Switch to WIN32_EXECUTABLE for releases.
else()
    target_compile_options(OrtaRecomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ── Recompiled game code (optional until you have generated it) ──────────

if(EXISTS "${PDO_GEN_DIR}/recomp_dispatch.c")
    message(STATUS "Orta Recomp: generated code found in ${PDO_GEN_DIR}")

    # Only the parts of xboxrecomp we use. Its graphics, audio and input
    # libraries are replaced by our own SDL3 ones (later milestones), so they
    # are not built at all.
    set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp")

    # Fixes we carry until they are merged upstream (patches/xboxrecomp/).
    # Applied once; re-running configure finds them already in place.
    find_package(Git QUIET)
    file(GLOB PDO_XBOXRECOMP_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/xboxrecomp/*.patch")
    list(SORT PDO_XBOXRECOMP_PATCHES)
    foreach(patch ${PDO_XBOXRECOMP_PATCHES})
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${patch}"
            WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
            RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
        if(NOT already EQUAL 0)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply "${patch}"
                WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
                RESULT_VARIABLE failed)
            if(failed)
                message(FATAL_ERROR "Could not apply ${patch} to external/xboxrecomp. "
                    "Reset it with: git -C external/xboxrecomp checkout -- .")
            endif()
            message(STATUS "Orta Recomp: applied ${patch}")
        endif()
    endforeach()
    add_subdirectory(${XBOXRECOMP_DIR}/src/platform ${CMAKE_BINARY_DIR}/xboxrecomp/platform)
    add_subdirectory(${XBOXRECOMP_DIR}/src/kernel ${CMAKE_BINARY_DIR}/xboxrecomp/kernel)

    file(GLOB PDO_GEN_SOURCES CONFIGURE_DEPENDS "${PDO_GEN_DIR}/*.c")
    # An object library rather than a static one: the kernel layer calls back
    # into the generated dispatch table and our hooks, a circular dependency
    # that single-pass linkers cannot resolve between static archives.
    add_library(pdo_recomp OBJECT ${PDO_GEN_SOURCES}
        src/guest/guest_manual.c
        src/guest/video_hooks.c
        src/guest/crash_report.c
        src/guest/d3d_hle.c
        src/guest/dsound_hle.c
        src/guest/cri_hle.c
        src/guest/input_hle.c
        src/guest/vsh.c
        src/guest/d3d_calltrace.c
        src/audio/mixer.c
        src/gfx/renderer.c
        src/gfx/overlay.c
        src/gfx/xtex.c)
    target_include_directories(pdo_recomp PUBLIC "${PDO_GEN_DIR}" src)
    target_link_libraries(pdo_recomp PUBLIC xbox_kernel SDL3::SDL3-static)
    if(WIN32)
        target_link_libraries(pdo_recomp PUBLIC dbghelp)
    endif()
    # The vertex program interpreter rounds every operation, as the NV2A
    # does: no fused multiply-add (fp:precise is MSVC's default already).
    if(NOT MSVC)
        set_source_files_properties(src/guest/vsh.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    endif()
    if(MSVC)
        target_compile_options(pdo_recomp PRIVATE /bigobj /w)
    else()
        # The generated code reads guest memory through casted pointers and
        # relies on 32-bit wraparound, so neither assumption may be optimised on.
        target_compile_options(pdo_recomp PRIVATE
            -fno-strict-aliasing -fwrapv -w)
    endif()

    target_sources(OrtaRecomp PRIVATE src/guest/guest.c)
    target_compile_definitions(OrtaRecomp PRIVATE PDO_HAVE_RECOMP=1)
    target_link_libraries(OrtaRecomp PRIVATE pdo_recomp)
else()
    message(STATUS "Orta Recomp: no generated code in ${PDO_GEN_DIR}; "
                   "building the launcher only (run tools/recomp.py first)")
endif()
