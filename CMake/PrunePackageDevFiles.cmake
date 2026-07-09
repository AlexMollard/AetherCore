cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required")
endif()

file(GLOB_RECURSE _dev_outputs
    "${PACKAGE_DIR}/*.exp"
    "${PACKAGE_DIR}/*.ilk"
    "${PACKAGE_DIR}/*.lib"
    "${PACKAGE_DIR}/*.pdb"
)
if(_dev_outputs)
    file(REMOVE ${_dev_outputs})
endif()
