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
    src/generic_compound_plan_guard.c
    src/generic_index_ddl.c
    src/generic_index_policy.c
    src/generic_index_sql.c
    src/generic_predicate.c
    src/generic_predicate_plan_sql.c
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
# layer owns generic WHERE semantics. The compound planner wraps the historical
# equality/range planner. The index layer can still promote proven single-term
# equality plans, while the final guard keeps compound plans aligned with the
# current scan-plus-residual execution path.
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
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range_base"
)
set_source_files_properties(
    src/generic_predicate_plan_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range;tinydb_generic_sql_build_select_plan_predicate_base=tinydb_generic_sql_build_select_plan_range_base;tinydb_generic_sql_print_plan_predicate_base=tinydb_generic_sql_print_plan_range_base"
)
set_source_files_properties(
    src/generic_index_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_index_base"
)

# The original multi-root policy intentionally allowed indexes only on root 0.
# Preserve it under a private symbol and expose the narrower schema-aware policy
# from generic_index_policy.c for generic fixed-slot table roots.
set_source_files_properties(
    src/multitable.c
    PROPERTIES COMPILE_DEFINITIONS
    "multitable_index_target_supported=multitable_index_target_supported_legacy"
)

# Keep the historical index catalog implementation as the storage delegate.
# generic_index_ddl.c validates non-users generic roots before calling it, so
# unsupported composite/unknown/primary-key generic index definitions never
# enter the catalog.
set_source_files_properties(
    src/table.c
    PROPERTIES COMPILE_DEFINITIONS
    "table_create_index=table_create_index_legacy"
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
