# The malgrind agent as an ESP-IDF component. malgrind adds this directory to EXTRA_COMPONENT_DIRS at build time,
# so the user's project does not change. Nothing references autostart.cpp, so `-u` forces its object into the link
# (IDF's WHOLE_ARCHIVE is not usable: PlatformIO places the --whole-archive markers around every component).
# IDF names a component after its directory, so the shared sources are wrapped here
# instead of registering core/ and platforms/esp32/ directly.
set(root "${CMAKE_CURRENT_LIST_DIR}/../../..")
idf_component_register(
    SRCS "${root}/core/src/profiler.cpp" "${root}/core/src/command.cpp" "${root}/platforms/esp32/src/esp32.cpp" "autostart.cpp"
    INCLUDE_DIRS "${root}/core/include" "${root}/platforms/esp32/include"
    REQUIRES driver esp_system esp_hw_support freertos heap
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u malgrind_autostart_link")