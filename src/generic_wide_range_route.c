#include "generic_predicate.h"
#include "generic_sql.h"

#include <ctype.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_predicate_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

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

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool legacy_fixed_row_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool parse_projection(TinyDBGenericParser* parser) {
    if (tinydb_generic_consume_char(parser, '*')) return true;

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    return tinydb_generic_parse_identifier(parser, column, sizeof(column));
}

static bool consume_limit_offset(TinyDBGenericParser* parser) {
    uint32_t ignored = 0u;
    if (tinydb_generic_consume_word(parser, "limit")) {
        if (!tinydb_generic_parse_uint32(parser, &ignored)) return false;
        if (tinydb_generic_consume_word(parser, "offset") &&
            !tinydb_generic_parse_uint32(parser, &ignored)) {
            return false;
        }
        return true;
    }
    if (tinydb_generic_consume_word(parser, "offset")) {
        return tinydb_generic_parse_uint32(parser, &ignored);
    }
    return true;
}

/*
 * The final schema-sized CRUD layer intentionally owns the fast equality
 * SELECT path because it can seek directly through the payload API.  Ordered
 * predicates, however, are implemented one layer deeper by
 * generic_range_select_sql.c.  Detect exactly that proven single-predicate
 * shape here so the equality-only parser cannot turn a valid wide range query
 * into a syntax error before the payload-native range executor sees it.
 */
static bool is_wide_single_range_select(Table* table, const char* sql) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select") ||
        !parse_projection(&parser) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        legacy_fixed_row_shape(schema) ||
        !tinydb_generic_consume_word(&parser, "where")) {
        return false;
    }

    TinyDBGenericPredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (!tinydb_generic_parse_predicate(&parser, schema, &predicate) ||
        predicate.op == TINYDB_GENERIC_COMPARE_EQ ||
        predicate.op == TINYDB_GENERIC_COMPARE_LIKE) {
        return false;
    }

    if (!consume_limit_offset(&parser)) return false;
    return tinydb_generic_consume_end(&parser);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    if (table != NULL && sql != NULL &&
        is_wide_single_range_select(table, sql)) {
        TinyDBGenericSqlStatus status =
            tinydb_generic_sql_try_execute_predicate_base(table, sql, result);
        if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;
    }

    return tinydb_generic_sql_try_execute_wide_base(table, sql, result);
}
