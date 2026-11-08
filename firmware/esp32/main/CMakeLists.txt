idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES mcu_flamegraph_core mcu_flamegraph_platform_esp32
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
