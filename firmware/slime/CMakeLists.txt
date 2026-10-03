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

# esp-dl compiles only the operators that components ask for. esp-sr asks for those of the wake
# word model; the face detector (human_face_detect) asks for nothing, so without this its model
# would lose Concat and others, fail to load, and the device would restart in a loop. Add its
# operators to the request (see tools/gen_espdl_req.py): the firmware gets the union of the two.
idf_build_get_property(slime_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST slime_components)
    idf_component_get_property(slime_espdl_dir espressif__esp-dl COMPONENT_DIR)
    idf_build_get_property(slime_python PYTHON)
    set(slime_espdl_req "${CMAKE_BINARY_DIR}/espdl_face_req.yml")
    execute_process(
        COMMAND ${slime_python} "${CMAKE_CURRENT_LIST_DIR}/tools/gen_espdl_req.py"
                "${slime_espdl_dir}/spec/kernels.yml" "${slime_espdl_req}"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "slime: could not write ${slime_espdl_req}")
    endif()
    idf_build_set_property(ESPDL_COMPILE_REQS "${slime_espdl_req}" APPEND)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/tools/gen_espdl_req.py")
endif()
