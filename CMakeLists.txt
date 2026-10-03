# torchlight - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(torchlight LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TORCHLIGHT_SOURCES
    src/main.cpp
    src/torchlight_installation.cpp
)

if(WIN32)
    add_executable(torchlight WIN32 ${TORCHLIGHT_SOURCES})
else()
    add_executable(torchlight ${TORCHLIGHT_SOURCES})
endif()

rexglue_setup_target(torchlight GPU_PLUGINS xenos)
