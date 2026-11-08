add_executable(host_runner
    main.cpp
)

target_link_libraries(host_runner PRIVATE 
    mcu_flamegraph_core 
    mcu_flamegraph_platform_host
)

target_compile_features(host_runner PRIVATE cxx_std_20)

