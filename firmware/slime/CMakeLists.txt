cmake_minimum_required(VERSION 3.16)

# Local patches to the BSP clone in ../../third_party (not under our version control).
# Idempotent: already-applied patches are skipped; failures only warn.
set(SLIME_BSP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp-mosaico-bsp")
foreach(patch bsp-lcd-psram-dma-direct.patch)
    set(p "${CMAKE_CURRENT_LIST_DIR}/patches/${patch}")
    execute_process(COMMAND git -C "${SLIME_BSP_DIR}" apply --reverse --check "${p}" RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
        execute_process(COMMAND git -C "${SLIME_BSP_DIR}" apply "${p}" RESULT_VARIABLE rc)
        if(rc EQUAL 0)
            message(STATUS "slime: applied ${patch}")
        else()
            message(WARNING "slime: could not apply ${patch} to ${SLIME_BSP_DIR}")
        endif()
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(slime_pet)
