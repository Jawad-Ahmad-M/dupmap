cmake_minimum_required(VERSION 3.16)
project(dupmap C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -O2)

find_package(Curses REQUIRED)
add_executable(dupmap src/main.c)
target_include_directories(dupmap PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(dupmap PRIVATE ${CURSES_LIBRARIES})
install(TARGETS dupmap RUNTIME DESTINATION bin)
install(FILES dupmap.1 DESTINATION share/man/man1)

enable_testing()
add_executable(dupmap_test_core tests/test_core.c)
target_include_directories(dupmap_test_core PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(dupmap_test_core PRIVATE ${CURSES_LIBRARIES})
add_test(NAME core_scan_and_layout COMMAND dupmap_test_core)

add_executable(dupmap_test_duplicates tests/test_duplicates.c)
target_include_directories(dupmap_test_duplicates PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(dupmap_test_duplicates PRIVATE ${CURSES_LIBRARIES})
add_test(NAME duplicate_detection COMMAND dupmap_test_duplicates)
