# boxsim's example: color bars, a dot where the screen is touched and a
# beep while it is, and a level that follows the BOX's roll.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The managed components differ by target: the codec's driver is the BOX's only.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
# Only what main needs: most of IDF has no linux port.
idf_build_set_property(MINIMAL_BUILD ON)
project(hello)
