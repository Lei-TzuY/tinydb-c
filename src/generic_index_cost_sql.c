#include "generic_index_candidates.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TINYDB_INDEX_COST_MIN_ROWS 32u
#define TINYDB_INDEX_COST_SCAN_CUTOFF_PERCENT 80u

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_single_anchor_uncosted_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_single_anchor_uncosted_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_single_anchor_uncosted_base(
    const TinyDBGenericSelectPlan* plan);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_range(const TinyDBGenericSelectPlan* plan);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (ci_char(*left) != ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static const char* find_keyword_ci(const char* sql, const char* keyword) {
    if (sql == NULL || keyword == NULL || keyword[0] == '\0') return NULL;
    size_t keyword_length = strlen(keyword);
    for (const char* p = sql; *p != '\0'; p++) {
        if (p != sql && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        size_t i = 0;
        while (i < keyword_length && p[i] != '\0' &&
               ci_char(p[i]) == ci_char(keyword[i])) {
            i++;
        }
        if (i != keyword_length) continue;
        if (isalnum((unsigned char)p[i]) || p[i] == '_') continue;
        return p;
    }
    return NULL;
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool plan_uses_single_secondary_source(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable || plan->index_name[0] == '\0') return false;
    return plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP ||
           plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE ||
           plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RESIDUAL;
}

static bool parse_index_predicates(const char* sql,
                                   const TableSchema* schema,
                                   const char* column_name,
                                   TinyDBGenericPredicate* predicates,
                                   uint32_t capacity,
                                   uint32_t* predicate_count) {
    *predicate_count = 0;
    const char* where = find_keyword_ci(sql, "where");
    if (where == NULL) return false;

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, where + 5);
    while (*predicate_count < capacity) {
        TinyDBGenericPredicate predicate;
        if (!tinydb_generic_parse_predicate(&parser, schema, &predicate)) {
            return false;
        }
        if (predicate.op <= TINYDB_GENERIC_COMPARE_LTE &&
            predicate.column_index < schema->num_columns &&
            ci_equal(schema->columns[predicate.column_index].name, column_name)) {
            predicates[(*predicate_count)++] = predicate;
        }
        if (!tinydb_generic_consume_word(&parser, "and")) break;
    }
    return *predicate_count > 0;
}

static bool estimate_plan_candidates(Table* table,
                                     const char* sql,
                                     const TinyDBGenericSelectPlan* plan,
                                     TinyDBGenericIndexEstimate* estimate) {
    memset(estimate, 0, sizeof(*estimate));
    if (!plan_uses_single_secondary_source(plan)) return false;
    if (find_keyword_ci(sql, "limit") != NULL ||
        find_keyword_ci(sql, "offset") != NULL) {
        return false;
    }

    TableSchema* schema = find_schema(table, plan->table_name);
    GenericSecondaryIndex* index = table_find_index_by_name(table, plan->index_name);
    if (schema == NULL || index == NULL) return false;

    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count = 0;
    if (!parse_index_predicates(sql,
                                schema,
                                plan->filter_column,
                                predicates,
                                MAX_COLUMNS_PER_TABLE,
                                &predicate_count)) {
        return false;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (predicate_count >= 2) {
        return tinydb_generic_index_estimate_conjunctive_candidates(
            table,
            schema,
            index,
            predicates,
            predicate_count,
            estimate,
            message,
            sizeof(message));
    }
    return tinydb_generic_index_estimate_candidates(table,
                                                     schema,
                                                     index,
                                                     &predicates[0],
                                                     estimate,
                                                     message,
                                                     sizeof(message));
}

static bool cost_prefers_scan(Table* table,
                              const char* sql,
                              const TinyDBGenericSelectPlan* plan,
                              TinyDBGenericIndexEstimate* estimate) {
    if (!estimate_plan_candidates(table, sql, plan, estimate)) return false;
    if (estimate->total_count < TINYDB_INDEX_COST_MIN_ROWS ||
        estimate->total_count == 0) {
        return false;
    }
    uint64_t candidate_percent =
        (uint64_t)estimate->candidate_count * 100u;
    uint64_t cutoff =
        (uint64_t)estimate->total_count * TINYDB_INDEX_COST_SCAN_CUTOFF_PERCENT;
    return candidate_percent > cutoff;
}

static bool build_uncosted_plan(Table* table,
                                const char* sql,
                                TinyDBGenericSelectPlan* plan) {
    TinyDBGenericSqlResult result;
    memset(&result, 0, sizeof(result));
    memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_single_anchor_uncosted_base(
               table, sql, plan, &result) == TINYDB_GENERIC_SQL_SUCCESS &&
           plan->applicable;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_single_anchor_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSelectPlan plan;
    TinyDBGenericIndexEstimate estimate;
    if (table != NULL && sql != NULL &&
        build_uncosted_plan(table, sql, &plan) &&
        cost_prefers_scan(table, sql, &plan, &estimate)) {
        TinyDBGenericSqlStatus scan_status =
            tinydb_generic_sql_try_execute_range_select(table, sql, result);
        if (scan_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
            return scan_status;
        }
    }
    return tinydb_generic_sql_try_execute_single_anchor_uncosted_base(
        table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_single_anchor_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSelectPlan uncosted;
    TinyDBGenericIndexEstimate estimate;
    if (table != NULL && sql != NULL && plan != NULL &&
        build_uncosted_plan(table, sql, &uncosted) &&
        cost_prefers_scan(table, sql, &uncosted, &estimate)) {
        TinyDBGenericSqlStatus scan_status =
            tinydb_generic_sql_build_select_plan_range(table, sql, plan, result);
        if (scan_status == TINYDB_GENERIC_SQL_SUCCESS && plan->applicable) {
            plan->cost_based_scan = true;
            plan->estimated_candidate_count = estimate.candidate_count;
            plan->estimated_total_count = estimate.total_count;
            plan->scan_cutoff_percent = TINYDB_INDEX_COST_SCAN_CUTOFF_PERCENT;
            snprintf(plan->rejected_index_name,
                     sizeof(plan->rejected_index_name),
                     "%s",
                     uncosted.index_name);
            return scan_status;
        }
    }
    return tinydb_generic_sql_build_select_plan_single_anchor_uncosted_base(
        table, sql, plan, result);
}

void tinydb_generic_sql_print_plan_single_anchor_base(
    const TinyDBGenericSelectPlan* plan) {
    if (plan != NULL && plan->applicable && plan->cost_based_scan) {
        tinydb_generic_sql_print_plan_range(plan);
        printf("  COST: rejected %s candidates=%u/%u > %u%% scan cutoff\n",
               plan->rejected_index_name,
               plan->estimated_candidate_count,
               plan->estimated_total_count,
               plan->scan_cutoff_percent);
        return;
    }
    tinydb_generic_sql_print_plan_single_anchor_uncosted_base(plan);
}
