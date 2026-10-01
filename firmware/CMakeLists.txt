cmake_minimum_required(VERSION 3.16)

if(NOT PROJECT_VER)
    set(PROJECT_VER "1")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emmaforo LANGUAGES C CXX)
