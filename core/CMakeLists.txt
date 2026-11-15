add_library(mulcan_core src/profiler.cpp)
target_include_directories(mulcan_core PUBLIC include)
target_compile_features(mulcan_core PUBLIC cxx_std_20)

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE mulcan_core)
target_compile_options(test_core PRIVATE -UNDEBUG) # the test IS its asserts
add_test(NAME core COMMAND test_core)
