#include "generic_sql.h"

#include <ctype.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_union_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_or_union_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_or_union_base(
    const TinyDBGenericSelectPlan* plan);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool starts_like_keyword(const char* current, const char* sql) {
    if (current[0] == '\0' || current[1] == '\0' ||
        current[2] == '\0' || current[3] == '\0') {
        return false;
    }
    return ci_char(current[0]) == 'l' &&
           ci_char(current[1]) == 'i' &&
           ci_char(current[2]) == 'k' &&
           ci_char(current[3]) == 'e' &&
           (current == sql || !is_identifier_char(current[-1])) &&
           !is_identifier_char(current[4]);
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
        if (!in_string && starts_like_keyword(current, sql)) return true;
        current++;
    }
    return false;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_select_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    if (sql != NULL && contains_like_keyword(sql)) {
        return tinydb_generic_sql_try_execute_or_scan_base(table, sql, result);
    }
    return tinydb_generic_sql_try_execute_or_union_base(table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_parenthesized_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    if (sql != NULL && contains_like_keyword(sql)) {
        return tinydb_generic_sql_build_select_plan_or_scan_base(
            table, sql, plan, result);
    }
    return tinydb_generic_sql_build_select_plan_or_union_base(
        table, sql, plan, result);
}

void tinydb_generic_sql_print_plan_parenthesized_base(
    const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_or_union_base(plan);
}
