add_library(malgrind_platform_cortexm STATIC src/cortexm.cpp)
target_include_directories(malgrind_platform_cortexm PUBLIC include)
target_link_libraries(malgrind_platform_cortexm PUBLIC malgrind_core)
target_compile_features(malgrind_platform_cortexm PUBLIC cxx_std_20)
target_compile_options(malgrind_platform_cortexm PRIVATE
    "$<IF:$<CXX_COMPILER_ID:MSVC>,/W3;/permissive-;/Zc:__cplusplus;/utf-8,-Wall;-Wextra>")
