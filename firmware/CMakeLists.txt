cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.1" CACHE STRING "Firmware version" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emmaforo LANGUAGES C CXX)
