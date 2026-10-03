cmake_minimum_required(VERSION 3.16)

# Local patches to the BSP clone in ../../third_party (not under our version control), applied
# in this order; each one builds on the ones before it. Idempotent: the last patch that is already
# in the tree is found first, and only the later ones are applied. Failures only warn.
set(SLIME_BSP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp-mosaico-bsp")
set(SLIME_BSP_PATCHES bsp-lcd-psram-dma-direct.patch bsp-lcd-queue-depth.patch)
set(slime_first_missing 0)
set(slime_i 0)
foreach(patch ${SLIME_BSP_PATCHES})
    math(EXPR slime_i "${slime_i} + 1")
    execute_process(COMMAND git -C "${SLIME_BSP_DIR}" apply --reverse --check "${CMAKE_CURRENT_LIST_DIR}/patches/${patch}"
                    RESULT_VARIABLE not_there OUTPUT_QUIET ERROR_QUIET)
    if(not_there EQUAL 0)
        set(slime_first_missing ${slime_i}) # this one is in (and so are the earlier ones it sits on)
    endif()
endforeach()
set(slime_i 0)
foreach(patch ${SLIME_BSP_PATCHES})
    if(slime_i GREATER_EQUAL slime_first_missing)
        execute_process(COMMAND git -C "${SLIME_BSP_DIR}" apply "${CMAKE_CURRENT_LIST_DIR}/patches/${patch}" RESULT_VARIABLE rc)
        if(rc EQUAL 0)
            message(STATUS "slime: applied ${patch}")
        else()
            message(WARNING "slime: could not apply ${patch} to ${SLIME_BSP_DIR}")
        endif()
    endif()
    math(EXPR slime_i "${slime_i} + 1")
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
