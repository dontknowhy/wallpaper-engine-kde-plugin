# Scene wallpapers via Almamu/linux-wallpaperengine (submodule under third_party/).
# Only the core renderer is built: no webhelper -> no CEF, no Chromium.
project(lwebackend)

find_package(PkgConfig REQUIRED)

# ---------------------------------------------------------------------------
# linux-wallpaperengine-core, built at build time by ExternalProject.
#
# The submodule is pinned to a commit of the `next-v2` branch, which exposes a
# stable C API (include/linux-wallpaperengine/*.h).
#
# Notes carried over from validation (see plan 5.3/5.5):
#  * The build MUST target `linux-wallpaperengine-core` explicitly. `all` also
#    builds OBJECT libraries that the WPBUILD_*=OFF switches are supposed to
#    skip, and those need CEF/argparse/pulseaudio headers.
#  * The upstream CMake package is incomplete (no INTERFACE_INCLUDE_DIRECTORIES,
#    spurious find_dependency(GLEW)), so an IMPORTED target is declared here and
#    the headers are taken straight from the submodule instead of the install
#    tree.
# ---------------------------------------------------------------------------
set(LWE_SRC    ${CMAKE_SOURCE_DIR}/third_party/linux-wallpaperengine)
set(LWE_BUILD  ${CMAKE_BINARY_DIR}/lwe-build)
set(LWE_PREFIX ${CMAKE_BINARY_DIR}/lwe-install)

if(NOT EXISTS ${LWE_SRC}/CMakeLists.txt)
    message(FATAL_ERROR
        "linux-wallpaperengine submodule not found at ${LWE_SRC}.\n"
        "Run: git submodule update --init --recursive")
endif()

include(ExternalProject)

# ---------------------------------------------------------------------------
# Local patches against the pinned upstream commit.
#
# The submodule is never modified in our git history: the patch files in
# src/backend_lwe/patches/ are applied to its working tree at build time (they
# show up as local modifications inside the submodule, revert with
# `git -C third_party/linux-wallpaperengine checkout .`).
#
# 0001: scene video textures advance one frame per rendered frame because mpv is
#       put into untimed mode, so the playback rate is render_fps / video_fps.
#       See plan 6.8.
# 0002: shader constants declared with a script (constantshadervalues[x].script,
#       e.g. values driven by engine.timeOfDay) were never queued with the
#       script engine, so the JSON default was used forever. Scene 2843295581
#       ("飞天果糖") blends its video with three time-of-day overlay videos and
#       rendered as a flat clear-colour fill because the three multiply
#       constants stayed at 1. See plan 6.9.
# 0003: Configuration::baseOnly / noSolidFinal / passLog were left
#       uninitialised, so the flags read as random garbage: effects were
#       randomly skipped (baseOnly), final passes randomly routed (noSolidFinal)
#       and pass logging randomly enabled (passLog). See plan 6.9.
# 0004: memory footprint of scene video textures. MemoryStreamProtocol copies
#       the whole video into a stream buffer that mpv reads from memory, yet the
#       header's own copy was kept alive, and mpv was left with its default
#       demuxer cache (demuxer-max-bytes 150MiB + demuxer-max-back-bytes 50MiB,
#       caching a stream that is already in RAM) plus an audio output for a
#       texture that is always muted. Scene 2843295581 (4 x 2560x1440@60fps
#       videos, 348MB of .tex data) went from 1.31GB to 0.77GB RSS. See plan 6.10.
#
# Applied twice on purpose:
#  * at configure time, so a pinned checkout that was reset (git submodule
#    update, a checkout inside the submodule, ...) is repaired on the next
#    build. The touched files are declared as configure dependencies so that a
#    reset actually triggers a reconfigure;
#  * as ExternalProject's PATCH_COMMAND for a fresh build directory.
# The applier is idempotent, so whichever runs second is a no-op.
# ---------------------------------------------------------------------------
set(LWE_PATCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/patches)
set(LWE_PATCHES
    0001-video-texture-real-time-playback.patch
    0002-scripted-shader-constants.patch
    0003-initialize-configuration-flags.patch
    0004-memory-footprint.patch
)
set(LWE_PATCH_COMMAND "")
foreach(patch IN LISTS LWE_PATCHES)
    set(LWE_PATCH_FILE ${LWE_PATCH_DIR}/${patch})
    if(NOT EXISTS ${LWE_PATCH_FILE})
        message(FATAL_ERROR "missing lwe patch file: ${LWE_PATCH_FILE}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -DPATCH_FILE=${LWE_PATCH_FILE}
            -DLWE_SRC=${LWE_SRC}
            -P ${LWE_PATCH_DIR}/apply-patch.cmake
        RESULT_VARIABLE LWE_PATCH_RESULT
    )
    if(NOT LWE_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "failed to apply lwe patch: ${patch}")
    endif()

    # Every file the patch touches becomes a configure dependency, so resetting
    # the submodule makes the next build reconfigure (and re-apply).
    file(STRINGS ${LWE_PATCH_FILE} LWE_PATCH_TARGETS REGEX "^\\+\\+\\+ ")
    foreach(target IN LISTS LWE_PATCH_TARGETS)
        string(REGEX REPLACE "^\\+\\+\\+ [ab]/" "" target ${target})
        if(NOT target STREQUAL "/dev/null")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LWE_SRC}/${target})
            list(APPEND LWE_PATCHED_FILES ${LWE_SRC}/${target})
        endif()
    endforeach()

    list(APPEND LWE_PATCH_COMMAND
        COMMAND ${CMAKE_COMMAND}
            -DPATCH_FILE=${LWE_PATCH_FILE}
            -DLWE_SRC=${LWE_SRC}
            -P ${LWE_PATCH_DIR}/apply-patch.cmake
    )
endforeach()

ExternalProject_Add(linux-wallpaperengine-core-ep
    SOURCE_DIR      ${LWE_SRC}
    BINARY_DIR      ${LWE_BUILD}
    DOWNLOAD_COMMAND ""
    PATCH_COMMAND   ${LWE_PATCH_COMMAND}
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${LWE_PREFIX}
        -DWPBUILD_CORE=ON
        -DWPBUILD_WEBHELPER=OFF
        -DWPBUILD_FRONTEND=OFF
        -DWPBUILD_DEVVIEWER=OFF
        -DWPBUILD_TESTS=OFF
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build <BINARY_DIR> --target linux-wallpaperengine-core
    INSTALL_COMMAND
        ${CMAKE_COMMAND} --install <BINARY_DIR> --prefix ${LWE_PREFIX}
    BUILD_BYPRODUCTS ${LWE_PREFIX}/lib/liblinux-wallpaperengine-core.so
    USES_TERMINAL_BUILD TRUE
)

# Rebuild the engine when a patched file changed (e.g. after the submodule was
# reset and the patch re-applied above), instead of trusting the step stamps.
if(LWE_PATCHED_FILES)
    ExternalProject_Add_StepDependencies(linux-wallpaperengine-core-ep build ${LWE_PATCHED_FILES})
endif()

add_library(linux-wallpaperengine-core SHARED IMPORTED GLOBAL)
set_target_properties(linux-wallpaperengine-core PROPERTIES
    IMPORTED_LOCATION ${LWE_PREFIX}/lib/liblinux-wallpaperengine-core.so
    INTERFACE_INCLUDE_DIRECTORIES "${LWE_SRC}/include"
)

# ---------------------------------------------------------------------------
# Qt glue: LweSceneViewer (QML type) backed by the core C API.
# ---------------------------------------------------------------------------
find_package(Qt${QT_MAJOR_VERSION} COMPONENTS Gui Quick Qml OpenGL REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_library(${PROJECT_NAME}
    STATIC
    qml_helper/LweSceneBackend.cpp
    qml_helper/LweSceneBackend.hpp
)
target_link_libraries(${PROJECT_NAME}
    PUBLIC
    Qt::Quick
    Qt::Gui
    Qt::OpenGL
    linux-wallpaperengine-core
)
target_include_directories(${PROJECT_NAME} PUBLIC qml_helper)

# Ensure the shared library exists before linking.
add_dependencies(${PROJECT_NAME} linux-wallpaperengine-core-ep)

# ---------------------------------------------------------------------------
# Install the engine library.
#
# The plugin links against liblinux-wallpaperengine-core.so.0, and upstream's
# ExternalProject install step puts it into the build tree (${LWE_PREFIX}/lib).
# Without this rule an installed plugin would keep looking for it in the build
# directory (and the SONAME has to exist by that exact name, since that is what
# is recorded in the plugin's DT_NEEDED).
#
# Upstream derives the file name from the git tag (0.0.1 at the pinned commit,
# SONAME ...so.0), so the directory is copied with the upstream layout instead
# of hardcoding a version: the versioned file plus the .so.<soname> and .so
# symlinks all end up in the destination either way.
# ---------------------------------------------------------------------------
install(DIRECTORY ${LWE_PREFIX}/lib/
    DESTINATION ${KDE_INSTALL_LIBDIR}
    FILES_MATCHING PATTERN "liblinux-wallpaperengine-core.so*"
)
