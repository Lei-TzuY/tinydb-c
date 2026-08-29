#include "generic_boolean.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_index_base(
    const TinyDBGenericSelectPlan* plan);

static int wide_plan_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_plan_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_plan_ci_char(*left) != wide_plan_ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static TableSchema* wide_plan_find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (wide_plan_ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool wide_plan_is_legacy_fixed_row_schema(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3u &&
           wide_plan_ci_equal(schema->columns[0].name, "id") &&
           wide_plan_ci_equal(schema->columns[1].name, "username") &&
           wide_plan_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TinyDBGenericSqlStatus wide_plan_execute_error(
    TinyDBGenericSqlResult* result,
    const char* message) {
    if (result == NULL) return TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

static TinyDBGenericSqlStatus wide_plan_syntax_error(
    TinyDBGenericSqlResult* result,
    const char* message) {
    if (result == NULL) return TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

static bool wide_plan_parse_projection(TinyDBGenericParser* parser,
                                       char* projection_name,
                                       size_t projection_name_size,
                                       bool* is_star,
                                       bool* is_count) {
    *is_star = false;
    *is_count = false;
    projection_name[0] = '\0';

    if (tinydb_generic_consume_char(parser, '*')) {
        *is_star = true;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *is_count = true;
        return true;
    }
    *parser = backup;

    return tinydb_generic_parse_identifier(
        parser, projection_name, projection_name_size);
}

static const TinyDBGenericPredicate* wide_plan_first_predicate(
    const TinyDBGenericBooleanExpression* expression) {
    for (uint32_t i = 0u; i < expression->count; i++) {
        if (expression->nodes[i].kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
            return &expression->nodes[i].predicate;
        }
    }
    return NULL;
}

static bool wide_plan_is_exact_pk_lookup(
    const TinyDBGenericBooleanExpression* expression) {
    if (expression->count == 0u ||
        expression->root >= expression->count ||
        expression->nodes[expression->root].kind != TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        return false;
    }
    const TinyDBGenericPredicate* predicate =
        &expression->nodes[expression->root].predicate;
    return predicate->column_index == 0u &&
           predicate->op == TINYDB_GENERIC_COMPARE_EQ;
}

static void wide_plan_format_filter_value(
    const TinyDBGenericPredicate* predicate,
    char* output,
    size_t output_size) {
    if (predicate->value.type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", predicate->value.int_value);
    } else {
        snprintf(output, output_size, "'%s'", predicate->value.text);
    }
}

static TinyDBGenericSqlStatus try_build_wide_payload_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    if (table == NULL || sql == NULL || plan == NULL) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char projection_name[MAX_NAME_SIZE];
    bool projection_star = false;
    bool projection_count = false;
    if (!wide_plan_parse_projection(&parser,
                                    projection_name,
                                    sizeof(projection_name),
                                    &projection_star,
                                    &projection_count)) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TableSchema* schema = wide_plan_find_schema(table, table_name);
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        wide_plan_is_legacy_fixed_row_schema(schema)) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(schema,
                                                schema_message,
                                                sizeof(schema_message))) {
        return wide_plan_execute_error(result, schema_message);
    }

    memset(plan, 0, sizeof(*plan));
    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
    plan->root_page_num = schema->root_page_num;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", schema->name);

    if (projection_star) {
        snprintf(plan->projection, sizeof(plan->projection), "*");
    } else if (projection_count) {
        snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
    } else {
        int projection_index =
            tinydb_generic_find_column_index(schema, projection_name);
        if (projection_index < 0) {
            return wide_plan_syntax_error(
                result,
                "generic SELECT projection references an unknown column");
        }
        snprintf(plan->projection,
                 sizeof(plan->projection),
                 "%s",
                 schema->columns[projection_index].name);
    }

    if (tinydb_generic_consume_end(&parser)) {
        if (result != NULL) {
            memset(result, 0, sizeof(*result));
            result->status = TINYDB_GENERIC_SQL_SUCCESS;
            result->statement_type = STATEMENT_SELECT;
            result->statement_type_valid = true;
            result->execute_result = EXECUTE_SUCCESS;
        }
        return TINYDB_GENERIC_SQL_SUCCESS;
    }

    if (!tinydb_generic_consume_word(&parser, "where")) {
        return wide_plan_syntax_error(
            result,
            "generic SELECT contains an unsupported clause");
    }

    TinyDBGenericBooleanExpression expression;
    if (!tinydb_generic_parse_boolean_expression(&parser, schema, &expression)) {
        return wide_plan_syntax_error(
            result,
            "generic SELECT WHERE requires a typed predicate expression");
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        uint32_t ignored_limit = 0u;
        if (!tinydb_generic_parse_uint32(&parser, &ignored_limit)) {
            return wide_plan_syntax_error(result, "LIMIT requires an integer");
        }
        if (tinydb_generic_consume_word(&parser, "offset")) {
            uint32_t ignored_offset = 0u;
            if (!tinydb_generic_parse_uint32(&parser, &ignored_offset)) {
                return wide_plan_syntax_error(result, "OFFSET requires an integer");
            }
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        uint32_t ignored_offset = 0u;
        if (!tinydb_generic_parse_uint32(&parser, &ignored_offset)) {
            return wide_plan_syntax_error(result, "OFFSET requires an integer");
        }
    }

    if (!tinydb_generic_consume_end(&parser)) {
        return wide_plan_syntax_error(
            result,
            "generic SELECT contains an unsupported clause after its predicates");
    }

    plan->has_filter = true;
    plan->kind = wide_plan_is_exact_pk_lookup(&expression)
        ? TINYDB_GENERIC_PLAN_PRIMARY_KEY_LOOKUP
        : TINYDB_GENERIC_PLAN_FULL_SCAN;
    if (!tinydb_generic_boolean_format(&expression,
                                       schema,
                                       plan->filter_expression,
                                       sizeof(plan->filter_expression))) {
        return wide_plan_syntax_error(result,
                                      "predicate plan text exceeds capacity");
    }

    const TinyDBGenericPredicate* first = wide_plan_first_predicate(&expression);
    if (first != NULL) {
        snprintf(plan->filter_column,
                 sizeof(plan->filter_column),
                 "%s",
                 schema->columns[first->column_index].name);
        snprintf(plan->filter_operator,
                 sizeof(plan->filter_operator),
                 "%s",
                 tinydb_generic_compare_op_text(first->op));
        wide_plan_format_filter_value(first,
                                      plan->filter_value,
                                      sizeof(plan->filter_value));
    }

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = TINYDB_GENERIC_SQL_SUCCESS;
        result->statement_type = STATEMENT_SELECT;
        result->statement_type_valid = true;
        result->execute_result = EXECUTE_SUCCESS;
    }
    return TINYDB_GENERIC_SQL_SUCCESS;
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlStatus wide_status =
        try_build_wide_payload_plan(table, sql, plan, result);
    if (wide_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
        return wide_status;
    }

    TinyDBGenericSqlStatus status = tinydb_generic_sql_build_select_plan_index_base(
        table, sql, plan, result);
    if (status == TINYDB_GENERIC_SQL_SUCCESS &&
        plan != NULL && plan->filter_expression[0] != '\0' &&
        plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP) {
        /*
         * The current generic secondary-index executor only proves a complete
         * single equality predicate. A compound AND plan may have an indexed
         * equality term, but the remaining terms still require decoded row
         * evaluation. Keep EXPLAIN aligned with the execution path until an
         * index-plus-residual-filter plan exists.
         */
        plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
        plan->index_name[0] = '\0';
    }
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_index_base(plan);
}
