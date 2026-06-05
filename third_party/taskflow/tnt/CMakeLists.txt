cmake_minimum_required(VERSION 3.19)
project(taskflow)

set(TARGET taskflow)
set(OUR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(${TARGET} INTERFACE)
target_include_directories(${TARGET} INTERFACE ${OUR_DIR})
set_target_properties(${TARGET} PROPERTIES FOLDER ThirdParty)
