cmake_minimum_required(VERSION 3.16)

# Register project-level components: Core engine and ESP32 Platform HAL
set(EXTRA_COMPONENT_DIRS 
    "${CMAKE_CURRENT_LIST_DIR}/../../core"
    "${CMAKE_CURRENT_LIST_DIR}/../../platforms/esp32"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_flamegraph_demo)
