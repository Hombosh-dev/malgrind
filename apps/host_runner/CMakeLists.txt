add_executable(host_runner main.cpp)
target_link_libraries(host_runner PRIVATE malgrind_core malgrind_platform_host)
# Static + non-PIE: every code address fits in 32 bits and matches the ELF, so addr2line works directly.
target_compile_options(host_runner PRIVATE -O1 -g -fno-pie)
target_link_options(host_runner PRIVATE -static -no-pie)
