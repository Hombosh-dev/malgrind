include(FetchContent)
FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# The version lives in the VERSION file at the repository root (scripts/version.mjs keeps the app's copies in step).
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION MALGRIND_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)

add_library(malgrind_host STATIC
    src/wire.cpp src/symbols.cpp src/aggregate.cpp src/process.cpp src/http.cpp src/source.cpp src/discover.cpp src/store.cpp src/session.cpp src/pio.cpp src/backup.cpp src/projects.cpp src/github.cpp src/boards.cpp src/ops.cpp src/serve.cpp src/cli.cpp)
target_include_directories(malgrind_host PUBLIC src)
target_link_libraries(malgrind_host PUBLIC malgrind_core nlohmann_json::nlohmann_json)
target_compile_features(malgrind_host PUBLIC cxx_std_20)
target_compile_definitions(malgrind_host PUBLIC MALGRIND_VERSION="${MALGRIND_VERSION}")
target_compile_options(malgrind_host PUBLIC -Wall -Wextra)

# The live page of the terminal mode is compiled into the binary as a raw string literal.
file(READ web/flame.html FLAME_HTML)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/flame_html.inc "R\"malgrind_html(${FLAME_HTML})malgrind_html\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS web/flame.html)
target_include_directories(malgrind_host PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Where `malgrind-cli run` finds agent/esp32/ unless told otherwise. A source build uses the repository; packaging sets its own.
set(MALGRIND_DEFAULT_AGENT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "directory that contains agent/esp32/")

# Named malgrind-cli: the app's own executable is called malgrind, and both end up in /usr/bin.
add_executable(malgrind-cli src/main.cpp)
target_compile_definitions(malgrind-cli PRIVATE MALGRIND_DEFAULT_AGENT_DIR="${MALGRIND_DEFAULT_AGENT_DIR}")
target_link_libraries(malgrind-cli PRIVATE malgrind_host)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # the packaged sidecar must not need a newer libstdc++ than the user's distribution has
    target_link_options(malgrind-cli PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_executable(test_host tests/test_host.cpp)
target_link_libraries(test_host PRIVATE malgrind_host)
target_compile_options(test_host PRIVATE -UNDEBUG)
add_test(NAME host COMMAND test_host)

add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE malgrind_host)
target_compile_options(test_store PRIVATE -UNDEBUG)
add_test(NAME store COMMAND test_store)

add_executable(test_session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE malgrind_host)
target_compile_options(test_session PRIVATE -UNDEBUG)
add_test(NAME session COMMAND test_session)

add_executable(test_backup tests/test_backup.cpp)
target_link_libraries(test_backup PRIVATE malgrind_host)
target_compile_options(test_backup PRIVATE -UNDEBUG)
add_test(NAME backup COMMAND test_backup)

add_executable(test_projects tests/test_projects.cpp)
target_link_libraries(test_projects PRIVATE malgrind_host)
target_compile_options(test_projects PRIVATE -UNDEBUG)
add_test(NAME projects COMMAND test_projects)

add_executable(test_github tests/test_github.cpp)
target_link_libraries(test_github PRIVATE malgrind_host)
target_compile_options(test_github PRIVATE -UNDEBUG)
add_test(NAME github COMMAND test_github)

add_executable(test_boards tests/test_boards.cpp)
target_link_libraries(test_boards PRIVATE malgrind_host)
target_compile_options(test_boards PRIVATE -UNDEBUG)
add_test(NAME boards COMMAND test_boards)

add_executable(test_flash tests/test_flash.cpp)
target_link_libraries(test_flash PRIVATE malgrind_host)
target_compile_options(test_flash PRIVATE -UNDEBUG)
target_compile_definitions(test_flash PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME flash COMMAND test_flash)

add_executable(test_pio tests/test_pio.cpp)
target_link_libraries(test_pio PRIVATE malgrind_host)
target_compile_options(test_pio PRIVATE -UNDEBUG)
target_compile_definitions(test_pio PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME pio COMMAND test_pio)

add_executable(test_serve tests/test_serve.cpp)
target_link_libraries(test_serve PRIVATE malgrind_host)
target_compile_options(test_serve PRIVATE -UNDEBUG)
target_compile_definitions(test_serve PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME serve COMMAND test_serve)

add_executable(test_cli tests/test_cli.cpp)
target_link_libraries(test_cli PRIVATE malgrind_host)
target_compile_options(test_cli PRIVATE -UNDEBUG)
add_test(NAME cli COMMAND test_cli)
