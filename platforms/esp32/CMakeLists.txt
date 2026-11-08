if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/esp32.cpp"
        INCLUDE_DIRS "include"
        REQUIRES mcu_flamegraph_core esp_timer esp_hw_support
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
endif()
