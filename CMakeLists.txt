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
    src/catalog_pragmas.c
    src/compiler.c
    src/diagnostics.c
    src/engine.c
    src/generic_index_sql.c
    src/generic_predicate.c
    src/generic_predicate_sql.c
    src/generic_range_mutation_sql.c
    src/generic_range_plan_sql.c
    src/generic_range_select_sql.c
    src/generic_sql.c
    src/join_plan.c
    src/multitable.c
    src/pager.c
    src/profile.c
    src/record.c
    src/table.c
    src/vm.c
)

# Keep the historical generic layers intact as fallbacks. The shared predicate
# layer owns generic WHERE semantics. The outer index layer only intercepts
# non-users equality SELECTs that have a schema-aware single-column index.
set_source_files_properties(
    src/generic_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_base"
)
set_source_files_properties(
    src/generic_range_mutation_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_mutations"
)
set_source_files_properties(
    src/generic_range_select_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_predicate_base"
)
set_source_files_properties(
    src/generic_predicate_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_select"
)
set_source_files_properties(
    src/generic_range_plan_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range"
)

add_library(tinydb_core STATIC ${TINYDB_CORE_SOURCES})
target_include_directories(tinydb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(tinydb src/main.c)
target_link_libraries(tinydb PRIVATE tinydb_core)

add_executable(tinydb_bench bench/benchmark.c)
target_link_libraries(tinydb_bench PRIVATE tinydb_core)

add_executable(tinydb_generic_bench bench/generic_benchmark.c)
target_link_libraries(tinydb_generic_bench PRIVATE tinydb_core)

add_executable(tinydb_pager_growth_probe tests/pager_growth_probe.c)
target_link_libraries(tinydb_pager_growth_probe PRIVATE tinydb_core)

add_executable(tinydb_engine_api_probe tests/engine_api_probe.c)
target_link_libraries(tinydb_engine_api_probe PRIVATE tinydb_core)

add_executable(tinydb_page_ownership_probe tests/page_ownership_probe.c)
target_link_libraries(tinydb_page_ownership_probe PRIVATE tinydb_core)

add_executable(tinydb_generic_record_probe tests/generic_record_probe.c)
target_link_libraries(tinydb_generic_record_probe PRIVATE tinydb_core)
