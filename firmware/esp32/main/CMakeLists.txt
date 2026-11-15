idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES mulcan esp_timer)
# a call in tail position would replace its caller's frame and hide it from the flamegraph
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-optimize-sibling-calls)
