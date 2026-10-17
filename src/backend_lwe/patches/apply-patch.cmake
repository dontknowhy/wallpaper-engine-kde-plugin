# Applies the linux-wallpaperengine patch series to the submodule working tree.
#
# Used at configure time (so a submodule that was reset is repaired) and as the
# ExternalProject PATCH_COMMAND (for a fresh build directory); whichever runs
# second finds the series applied and does nothing.
#
# The tree can be in one of three states:
#  * the series is not applied     -> apply it;
#  * the series is already applied -> nothing to do;
#  * anything else (a partially applied tree, a patch added to the series while
#    the tree still has the previous ones, hand edits) -> reset the working tree
#    to the pinned commit and apply the series. The submodule is never modified in
#    our git history, so a reset cannot lose anything of ours; whatever was in
#    the tree is logged, in case it was an intentional local edit.
#
# The patches are bundled into a single file before being handed to git, and the
# bundle is always used as a whole: git apply can chain hunks of one patch file,
# but it reverses each patch file independently, so a per-patch "already applied"
# check gives false negatives as soon as two patches touch nearby lines (0005 and
# 0006 do).
#
# Usage:
#   cmake -DLWE_SRC=<dir> -DPATCH_DIR=<dir> -DPATCHES=a.patch:b.patch \
#         -DPATCH_BUNDLE=<file> -P apply-patch.cmake

find_program(GIT_EXECUTABLE git REQUIRED)

if(NOT IS_DIRECTORY "${LWE_SRC}")
    message(FATAL_ERROR "lwe: source directory not found: ${LWE_SRC}")
endif()
if(NOT PATCHES)
    message(FATAL_ERROR "lwe: no patches given")
endif()

# The series arrives as one ":"-joined argument (a CMake list would be split into
# several arguments when passed through -D).
string(REPLACE ":" ";" PATCHES "${PATCHES}")

set(patchFiles "")
foreach(patch IN LISTS PATCHES)
    set(file "${PATCH_DIR}/${patch}")

    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "lwe: patch file not found: ${file}")
    endif()

    list(APPEND patchFiles "${file}")
endforeach()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E cat ${patchFiles}
    OUTPUT_FILE "${PATCH_BUNDLE}"
    RESULT_VARIABLE bundleResult
)
if(NOT bundleResult EQUAL 0 OR NOT EXISTS "${PATCH_BUNDLE}")
    message(FATAL_ERROR "lwe: cannot bundle the patch series into ${PATCH_BUNDLE}")
endif()

# Nothing applied yet: apply the whole bundle.
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --check "${PATCH_BUNDLE}"
    WORKING_DIRECTORY "${LWE_SRC}"
    RESULT_VARIABLE forwardCheck
    OUTPUT_QUIET ERROR_QUIET
)
if(forwardCheck EQUAL 0)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply "${PATCH_BUNDLE}"
        WORKING_DIRECTORY "${LWE_SRC}"
        RESULT_VARIABLE applyResult
        OUTPUT_VARIABLE applyOutput
        ERROR_VARIABLE applyOutput
    )
    if(NOT applyResult EQUAL 0)
        message(FATAL_ERROR "lwe: failed to apply the patch series:\n${applyOutput}")
    endif()

    list(LENGTH patchFiles patchCount)
    message(STATUS "lwe: applied ${patchCount} patches")
    return()
endif()

# Everything applied: the bundle must also reverse cleanly.
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${PATCH_BUNDLE}"
    WORKING_DIRECTORY "${LWE_SRC}"
    RESULT_VARIABLE reverseCheck
    OUTPUT_QUIET ERROR_QUIET
)
if(reverseCheck EQUAL 0)
    message(STATUS "lwe: patch series already applied")
    return()
endif()

# Anything else: rebuild the tree from the pinned commit.
execute_process(
    COMMAND ${GIT_EXECUTABLE} status --short
    WORKING_DIRECTORY "${LWE_SRC}"
    OUTPUT_VARIABLE localChanges
)
execute_process(
    COMMAND ${GIT_EXECUTABLE} checkout -- .
    WORKING_DIRECTORY "${LWE_SRC}"
    RESULT_VARIABLE resetResult
    OUTPUT_VARIABLE resetOutput
    ERROR_VARIABLE resetOutput
)
if(NOT resetResult EQUAL 0)
    message(FATAL_ERROR "lwe: cannot reset the submodule working tree:\n${resetOutput}")
endif()

execute_process(
    COMMAND ${GIT_EXECUTABLE} apply "${PATCH_BUNDLE}"
    WORKING_DIRECTORY "${LWE_SRC}"
    RESULT_VARIABLE applyResult
    OUTPUT_VARIABLE applyOutput
    ERROR_VARIABLE applyOutput
)
if(NOT applyResult EQUAL 0)
    message(FATAL_ERROR "lwe: failed to apply the patch series after a reset:\n${applyOutput}")
endif()

string(STRIP "${localChanges}" localChanges)
if(localChanges STREQUAL "")
    message(STATUS "lwe: re-applied the patch series on a clean submodule")
else()
    message(STATUS "lwe: reset the submodule working tree and re-applied the patch series "
                   "(the tree contained:\n${localChanges})")
endif()
