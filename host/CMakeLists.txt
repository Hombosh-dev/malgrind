add_library(malgrind_host STATIC
    src/wire.cpp src/symbols.cpp src/aggregate.cpp src/process.cpp src/http.cpp src/source.cpp src/discover.cpp)
target_include_directories(malgrind_host PUBLIC src)
target_link_libraries(malgrind_host PUBLIC malgrind_core)
target_compile_features(malgrind_host PUBLIC cxx_std_20)
target_compile_options(malgrind_host PUBLIC -Wall -Wextra)

# The viewer page is compiled into the binary as a raw string literal.
file(READ web/flame.html FLAME_HTML)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/flame_html.inc "R\"malgrind_html(${FLAME_HTML})malgrind_html\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS web/flame.html)

add_executable(malgrind src/main.cpp)
target_include_directories(malgrind PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(malgrind PRIVATE malgrind_host)

add_executable(test_host tests/test_host.cpp)
target_link_libraries(test_host PRIVATE malgrind_host)
target_compile_options(test_host PRIVATE -UNDEBUG)
add_test(NAME host COMMAND test_host)
