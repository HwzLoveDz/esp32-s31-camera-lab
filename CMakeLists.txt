cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "Activate an ESP-IDF environment with ESP32-S31 support first. See docs/BUILD.md.")
endif()

set(SDKCONFIG_DEFAULTS "components/gen_bmgr_codes/board_manager.defaults;sdkconfig.defaults")
set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camera_lab)

# ESP-DL 3.3.11 uses anonymous typedefs with C++ default initializers.
# GCC 16 adds this diagnostic; retain the warning without making it fatal.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    idf_build_set_property(COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=non-c-typedef-for-linkage>" APPEND)
endif()
