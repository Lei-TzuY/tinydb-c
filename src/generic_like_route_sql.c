#include "generic_sql.h"

#include <ctype.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_like_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_like_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_like_base(
    const TinyDBGenericSelectPlan* plan);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool contains_like_keyword(const char* sql) {
    bool in_string = false;
    const char* current = sql;

    while (*current != '\0') {
        if (*current == '\'') {
            if (in_string && current[1] == '\'') {
                current += 2;
                continue;
            }
            in_string = !in_string;
            current++;
            continue;
        }

        if (!in_string &&
            ci_char(current[0]) == 'l' &&
            ci_char(current[1]) == 'i' &&
            ci_char(current[2]) == 'k' &&
            ci_char(current[3]) == 'e' &&
            (current == sql || !is_identifier_char(current[-1])) &&
            !is_identifier_char(current[4])) {
            return true;
        }
        current++;
    }
    return false;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    if (sql != NULL && contains_like_keyword(sql)) {
        /* LIKE is schema-aware residual filtering, not an ordered range. */
        return tinydb_generic_sql_try_execute_range_select(table, sql, result);
    }
    return tinydb_generic_sql_try_execute_like_base(table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    if (sql != NULL && contains_like_keyword(sql)) {
        return tinydb_generic_sql_build_select_plan_range(table, sql, plan, result);
    }
    return tinydb_generic_sql_build_select_plan_like_base(table, sql, plan, result);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_like_base(plan);
}
