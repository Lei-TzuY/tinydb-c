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
    src/generic_boolean.c
    src/generic_compound_plan_guard.c
    src/generic_index_anchor_sql.c
    src/generic_index_ddl.c
    src/generic_index_drop.c
    src/generic_index_epoch.c
    src/generic_index_policy.c
    src/generic_index_range_sql.c
    src/generic_index_record_guard.c
    src/generic_index_snapshot_sql.c
    src/generic_index_sql.c
    src/generic_or_mutation_sql.c
    src/generic_or_sql.c
    src/generic_parenthesized_sql.c
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
# layer owns typed comparisons. Equality/range indexes remain the proven
# single-term paths. The compound-anchor layer sits immediately above them and
# can narrow a flat AND expression with one indexed term before evaluating the
# residual predicates. OR and parenthesized boolean handling stay outside that
# layer and continue to fail closed to their established scan evaluators.
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
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_index_base"
)
set_source_files_properties(
    src/generic_index_snapshot_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_snapshot_base"
)
if(NOT MSVC)
    # capacity is uint32_t, so the SIZE_MAX guard is meaningful only on
    # narrower size_t targets; GCC 64-bit diagnoses the portable guard as a
    # constant-false comparison. Keep every other warning as an error.
    set_source_files_properties(
        src/generic_index_snapshot_sql.c
        PROPERTIES COMPILE_OPTIONS "-Wno-type-limits"
    )
endif()
set_source_files_properties(
    src/generic_index_range_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range_index_base"
)
set_source_files_properties(
    src/generic_index_anchor_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_anchor_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_anchor_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_anchor_base"
)
set_source_files_properties(
    src/generic_or_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_or_select_base;tinydb_generic_sql_try_execute_index_base=tinydb_generic_sql_try_execute_anchor_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_parenthesized_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_parenthesized_base;tinydb_generic_sql_build_select_plan_or_base=tinydb_generic_sql_build_select_plan_anchor_base;tinydb_generic_sql_print_plan_or_base=tinydb_generic_sql_print_plan_anchor_base"
)
set_source_files_properties(
    src/generic_or_mutation_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_parenthesized_base"
)
set_source_files_properties(
    src/generic_compound_plan_guard.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_compound_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_compound_base"
)

# Generic record mutation APIs are wrapped so a durable index epoch is advanced
# before indexed-table data changes. This makes equality/range snapshots fail
# closed after commit, rollback, crash recovery, direct API mutation, or REPL mutation.
set_source_files_properties(
    src/record.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_insert=tinydb_record_insert_base;tinydb_record_update=tinydb_record_update_base;tinydb_record_delete=tinydb_record_delete_base;tinydb_record_delete_all=tinydb_record_delete_all_base"
)

# The original multi-root policy intentionally allowed indexes only on root 0.
# Preserve it under a private symbol and expose the narrower schema-aware policy
# from generic_index_policy.c for generic fixed-slot table roots.
set_source_files_properties(
    src/multitable.c
    PROPERTIES COMPILE_DEFINITIONS
    "multitable_index_target_supported=multitable_index_target_supported_legacy"
)

# Keep historical index catalog/drop implementations as delegates. Generic DDL
# validates non-users roots before creation; the drop wrapper additionally
# removes the typed range sidecar so no orphan snapshot remains.
set_source_files_properties(
    src/table.c
    PROPERTIES COMPILE_DEFINITIONS
    "table_create_index=table_create_index_legacy;table_drop_index=table_drop_index_legacy"
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
