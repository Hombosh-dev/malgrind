if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/profiler.cpp"
        INCLUDE_DIRS "include"
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
else()
    add_library(mcu_flamegraph_core
        src/profiler.cpp
    )
    target_include_directories(mcu_flamegraph_core PUBLIC include)
    target_compile_features(mcu_flamegraph_core PUBLIC cxx_std_20)
endif()
