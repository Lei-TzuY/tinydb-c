cmake_minimum_required(VERSION 3.10)
project(TinyDB C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True)

# Enable all warnings
if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

set(TINYDB_CORE_SOURCES
    src/compiler.c
    src/pager.c
    src/profile.c
    src/table.c
    src/vm.c
)

add_library(tinydb_core STATIC ${TINYDB_CORE_SOURCES})
target_include_directories(tinydb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(tinydb src/main.c)
target_link_libraries(tinydb PRIVATE tinydb_core)

add_executable(tinydb_bench bench/benchmark.c)
target_link_libraries(tinydb_bench PRIVATE tinydb_core)

add_executable(tinydb_pager_growth_probe tests/pager_growth_probe.c)
target_link_libraries(tinydb_pager_growth_probe PRIVATE tinydb_core)
