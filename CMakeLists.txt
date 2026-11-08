cmake_minimum_required(VERSION 3.20)
project(mcu_flamegraph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-agnostic Core library (Pure C++20, zero target dependencies)
add_subdirectory(core)

# Platform backends
add_subdirectory(platforms/host)

# Native Host Applications (CLI / Demo Runner)
add_subdirectory(apps/host_runner)
