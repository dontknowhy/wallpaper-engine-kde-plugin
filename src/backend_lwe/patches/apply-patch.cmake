# Applies one patch file inside the linux-wallpaperengine submodule.
#
# Used as the ExternalProject PATCH_COMMAND so the submodule stays pinned to the
# upstream commit while we carry the few fixes we need locally (see plan 6.8).
#
# Idempotent: the patch step is stamped by ExternalProject, but a fresh build
# directory over an already-patched source tree (or a cold rebuild) would run it
# again, so "already applied" is treated as success.
#
# Usage: cmake -DPATCH_FILE=<file> -DLWE_SRC=<dir> -P apply-patch.cmake

if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "patch file not found: ${PATCH_FILE}")
endif()
if(NOT IS_DIRECTORY "${LWE_SRC}")
    message(FATAL_ERROR "source directory not found: ${LWE_SRC}")
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

# git apply --reverse --check succeeds only when the change is already there.
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${LWE_SRC}"
    RESULT_VARIABLE reverseCheck
    OUTPUT_QUIET ERROR_QUIET
)
if(reverseCheck EQUAL 0)
    message(STATUS "lwe: patch already applied: ${PATCH_FILE}")
    return()
endif()

execute_process(
    COMMAND ${GIT_EXECUTABLE} apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${LWE_SRC}"
    RESULT_VARIABLE applyResult
    OUTPUT_VARIABLE applyOutput
    ERROR_VARIABLE applyOutput
)
if(NOT applyResult EQUAL 0)
    message(FATAL_ERROR "lwe: failed to apply ${PATCH_FILE}:\n${applyOutput}")
endif()

message(STATUS "lwe: applied patch: ${PATCH_FILE}")
