# IDF names a component after its directory, so the shared sources are wrapped here
# instead of registering core/ and platforms/esp32/ directly.
set(root "${CMAKE_CURRENT_LIST_DIR}/../../../..")
idf_component_register(
    SRCS "${root}/core/src/profiler.cpp" "${root}/platforms/esp32/src/esp32.cpp"
    INCLUDE_DIRS "${root}/core/include" "${root}/platforms/esp32/include"
    REQUIRES driver esp_system esp_hw_support freertos
)
