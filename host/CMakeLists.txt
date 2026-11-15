add_library(mulcan_host STATIC
    src/wire.cpp src/symbols.cpp src/aggregate.cpp src/process.cpp src/http.cpp src/source.cpp src/discover.cpp)
target_include_directories(mulcan_host PUBLIC src)
target_link_libraries(mulcan_host PUBLIC mulcan_core)
target_compile_features(mulcan_host PUBLIC cxx_std_20)
target_compile_options(mulcan_host PUBLIC -Wall -Wextra)

# The viewer page is compiled into the binary as a raw string literal.
file(READ web/flame.html FLAME_HTML)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/flame_html.inc "R\"mulcan_html(${FLAME_HTML})mulcan_html\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS web/flame.html)

add_executable(mulcan src/main.cpp)
target_include_directories(mulcan PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mulcan PRIVATE mulcan_host)

add_executable(test_host tests/test_host.cpp)
target_link_libraries(test_host PRIVATE mulcan_host)
target_compile_options(test_host PRIVATE -UNDEBUG)
add_test(NAME host COMMAND test_host)
