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
    src/create_table_policy.c
    src/diagnostics.c
    src/engine.c
    src/generic_boolean.c
    src/generic_compound_plan_guard.c
    src/generic_index_anchor_v2_sql.c
    src/generic_index_candidates.c
    src/generic_index_stats.c
    src/generic_index_estimate_policy.c
    src/generic_index_ddl.c
    src/generic_index_drop.c
    src/generic_index_epoch.c
    src/generic_index_intersection_sql.c
    src/generic_index_policy.c
    src/generic_index_range_v2_sql.c
    src/generic_index_single_cost_sql.c
    src/generic_index_record_guard.c
    src/generic_index_snapshot_sql.c
    src/generic_index_sql.c
    src/generic_index_union_sql.c
    src/generic_index_union_cost_sql.c
    src/generic_like_route_sql.c
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
    src/multi_vacuum_into.c
    src/multitable.c
    src/schema_catalog_v2.c
    src/pager.c
    src/prepared_route.c
    src/profile.c
    src/record.c
    src/table.c
    src/vm.c
)

# Keep the historical engine body private behind facade wrappers. CREATE TABLE
# fixed-slot validation stays inside create_table_policy.c. Multi-table
# VACUUM INTO is a logical-rebuild facade inside prepared_route.c, so bound
# prepared VACUUM statements still re-enter the same safety pipeline.
set_source_files_properties(
    src/engine.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_execute_sql=tinydb_execute_sql_base"
)
set_source_files_properties(
    src/create_table_policy.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_execute_sql=tinydb_execute_sql_create_policy_base"
)
set_source_files_properties(
    src/multi_vacuum_into.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_execute_sql=tinydb_execute_sql_prepared_delegate_base"
)

# Keep historical generic layers as fallbacks while sharing ordered-index
# snapshot/checksum/epoch/bounds logic in generic_index_candidates.c. Exact
# estimates remain available as a private fallback. generic_index_stats.c owns
# the persisted synopsis estimator, while generic_index_estimate_policy.c keeps
# small tables on exact costing and uses synopsis estimates only once loading a
# full typed snapshot is materially more expensive.
set_source_files_properties(
    src/generic_index_candidates.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_index_estimate_candidates=tinydb_generic_index_estimate_candidates_exact_base;tinydb_generic_index_estimate_conjunctive_candidates=tinydb_generic_index_estimate_conjunctive_candidates_exact_base"
)
set_source_files_properties(
    src/generic_index_stats.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_index_estimate_candidates=tinydb_generic_index_estimate_candidates_stats_base;tinydb_generic_index_estimate_conjunctive_candidates=tinydb_generic_index_estimate_conjunctive_candidates_stats_base"
)

# Generic SELECT routing is deliberately ordered as follows:
#   parenthesized -> OR mutation wrapper -> LIKE router -> costed OR index union
#   -> OR scan -> flat AND intersection/anchor cost planner
#   -> costed single range/equality -> residual.
# LIKE therefore never enters an ordered index layer. Flat OR uses index union
# only when every disjunct has a proven PK or ordered secondary-index anchor and
# the estimated union work is not more expensive than the schema-aware scan.
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
    set_source_files_properties(
        src/generic_index_snapshot_sql.c
        PROPERTIES COMPILE_OPTIONS "-Wno-type-limits"
    )
endif()
set_source_files_properties(
    src/generic_index_range_v2_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_index_uncosted_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range_index_uncosted_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range_index_uncosted_base"
)
set_source_files_properties(
    src/generic_index_anchor_v2_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_single_anchor_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_single_anchor_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_single_anchor_base"
)
set_source_files_properties(
    src/generic_index_intersection_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_anchor_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_anchor_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_anchor_index_base"
)
set_source_files_properties(
    src/generic_or_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_or_scan_base;tinydb_generic_sql_try_execute_index_base=tinydb_generic_sql_try_execute_anchor_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_or_scan_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_or_scan_base;tinydb_generic_sql_build_select_plan_or_base=tinydb_generic_sql_build_select_plan_anchor_index_base;tinydb_generic_sql_print_plan_or_base=tinydb_generic_sql_print_plan_anchor_index_base"
)
set_source_files_properties(
    src/generic_index_union_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute_or_select_base=tinydb_generic_sql_try_execute_union_uncosted_base;tinydb_generic_sql_build_select_plan_parenthesized_base=tinydb_generic_sql_build_select_plan_union_uncosted_base;tinydb_generic_sql_print_plan_parenthesized_base=tinydb_generic_sql_print_plan_union_uncosted_base"
)
set_source_files_properties(
    src/generic_like_route_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_or_select_base;tinydb_generic_sql_try_execute_like_base=tinydb_generic_sql_try_execute_union_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_parenthesized_base;tinydb_generic_sql_build_select_plan_like_base=tinydb_generic_sql_build_select_plan_union_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_parenthesized_base;tinydb_generic_sql_print_plan_like_base=tinydb_generic_sql_print_plan_union_base"
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

# Preserve historical schema-catalog I/O as a V1 read fallback while the public
# facade writes a fixed little-endian V2 envelope with a payload checksum. Keep
# the historical root-0-only index policy private as before.
set_source_files_properties(
    src/multitable.c
    PROPERTIES COMPILE_DEFINITIONS
    "multitable_catalog_load=multitable_catalog_load_v1_base;multitable_catalog_save=multitable_catalog_save_v1_base;multitable_index_target_supported=multitable_index_target_supported_legacy"
)

# Keep historical index catalog/drop implementations as delegates. Generic DDL
# validates non-users roots before creation; drop also removes typed snapshots
# and optimizer-statistics sidecars.
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
