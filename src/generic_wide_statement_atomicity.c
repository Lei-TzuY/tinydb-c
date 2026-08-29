#include "generic_boolean.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WIDE_STATEMENT_SAVEPOINT_PREFIX "__tinydb_wide_stmt_"

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_grouped_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef enum {
    WIDE_STATEMENT_NONE = 0,
    WIDE_STATEMENT_UPDATE,
    WIDE_STATEMENT_DELETE
} WideStatementMutationKind;

static int wide_statement_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_statement_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_statement_ci_char(*left) != wide_statement_ci_char(*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool wide_statement_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_statement_ci_equal(schema->columns[0].name, "id") &&
           wide_statement_ci_equal(schema->columns[1].name, "username") &&
           wide_statement_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* wide_statement_find_schema(Table* table,
                                               const char* table_name) {
    if (table == NULL || table_name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_statement_ci_equal(table->catalog.schemas[i].name,
                                    table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static WideStatementMutationKind wide_statement_target(
    Table* table,
    const char* sql,
    TableSchema** out_schema) {
    if (out_schema != NULL) *out_schema = NULL;
    if (table == NULL || sql == NULL) return WIDE_STATEMENT_NONE;

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    WideStatementMutationKind kind = WIDE_STATEMENT_NONE;
    if (tinydb_generic_consume_word(&parser, "update")) {
        kind = WIDE_STATEMENT_UPDATE;
    } else if (tinydb_generic_consume_word(&parser, "delete") &&
               tinydb_generic_consume_word(&parser, "from")) {
        kind = WIDE_STATEMENT_DELETE;
    } else {
        return WIDE_STATEMENT_NONE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return WIDE_STATEMENT_NONE;
    }
    TableSchema* schema = wide_statement_find_schema(table, table_name);
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        wide_statement_legacy_shape(schema)) {
        return WIDE_STATEMENT_NONE;
    }
    if (out_schema != NULL) *out_schema = schema;
    return kind;
}

static TinyDBGenericSqlStatus wide_statement_atomicity_error(
    TinyDBGenericSqlResult* result,
    WideStatementMutationKind kind,
    const char* message) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = kind == WIDE_STATEMENT_DELETE
        ? STATEMENT_DELETE
        : STATEMENT_UPDATE;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message
                             : "unable to preserve wide statement atomicity");
    return result->status;
}

static bool wide_statement_savepoint_name(Pager* pager,
                                          char* name,
                                          size_t name_size) {
    if (pager == NULL || name == NULL || name_size == 0u) return false;
    for (uint32_t attempt = 0u; attempt <= MAX_SAVEPOINTS; attempt++) {
        snprintf(name,
                 name_size,
                 WIDE_STATEMENT_SAVEPOINT_PREFIX "%u_%u",
                 pager->savepoint_count,
                 attempt);
        bool duplicate = false;
        for (uint32_t i = 0u; i < pager->savepoint_count; i++) {
            if (strcmp(pager->savepoints[i].name, name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) return true;
    }
    return false;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;

    TableSchema* schema = NULL;
    WideStatementMutationKind kind = wide_statement_target(table, sql, &schema);
    (void)schema;
    if (kind == WIDE_STATEMENT_NONE || table == NULL || table->pager == NULL ||
        !table->in_transaction) {
        return tinydb_generic_sql_try_execute_wide_grouped_base(table,
                                                                sql,
                                                                output);
    }

    if (!table->pager->in_transaction) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "wide statement transaction state is inconsistent");
    }

    char savepoint_name[64];
    if (!wide_statement_savepoint_name(table->pager,
                                       savepoint_name,
                                       sizeof(savepoint_name)) ||
        !pager_savepoint(table->pager, savepoint_name)) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "unable to reserve a statement savepoint for wide mutation");
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_try_execute_wide_grouped_base(table, sql, output);
    if (status == TINYDB_GENERIC_SQL_SUCCESS) {
        if (!pager_release_savepoint(table->pager, savepoint_name)) {
            return wide_statement_atomicity_error(
                output,
                kind,
                "unable to release successful wide statement savepoint");
        }
        return status;
    }

    if (!pager_rollback_to_savepoint(table->pager, savepoint_name)) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "unable to roll back failed wide statement to its savepoint");
    }
    if (!pager_release_savepoint(table->pager, savepoint_name)) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "unable to release failed wide statement savepoint");
    }
    return status;
}
