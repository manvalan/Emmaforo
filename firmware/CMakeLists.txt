cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.2" CACHE STRING "Firmware version" FORCE)
# The power debug build (tools/debug_power.sh) must never pass for a release.
if(SDKCONFIG_DEFAULTS MATCHES "sdkconfig.debug_power")
    set(PROJECT_VER "1.2-debug" CACHE STRING "Firmware version" FORCE)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emmaforo LANGUAGES C CXX)
