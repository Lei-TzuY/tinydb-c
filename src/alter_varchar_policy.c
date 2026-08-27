#include "column_type.h"
#include "engine.h"
#include "multitable.h"
#include "record.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_alter_delegate_base(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result);

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
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool fixed_row_shape(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3u &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static void initialize_result(TinyDBSqlResult* result, TinyDBSqlStatus status) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = status;
    result->prepare_result = PREPARE_SUCCESS;
    result->execute_result = EXECUTE_SUCCESS;
    result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    result->statement_type = STATEMENT_ALTER_TABLE;
    result->statement_type_valid = true;
}

static TinyDBSqlStatus fail_result(TinyDBSqlResult* result,
                                   TinyDBSqlStatus status,
                                   const char* message) {
    initialize_result(result, status);
    if (result != NULL) {
        result->executed = false;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return status;
}

TinyDBSqlStatus tinydb_execute_sql_prepared_delegate_base(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result) {
    if (database == NULL || database->table == NULL || sql == NULL) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }

    Statement statement;
    memset(&statement, 0, sizeof(statement));
    if (prepare_statement(sql, &statement) != PREPARE_SUCCESS ||
        statement.type != STATEMENT_ALTER_TABLE ||
        !statement.alter_table.is_add_column) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }

    TinyDBColumnTypeSpec type;
    if (!tinydb_column_type_parse(statement.alter_table.new_col_type, &type) ||
        type.type != COL_TYPE_VARCHAR ||
        !type.explicitly_sized) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }

    Table* table = database->table;
    if (table->in_transaction) {
        return fail_result(result,
                           TINYDB_SQL_POLICY_ERROR,
                           "schema DDL is not allowed inside a transaction");
    }

    TableSchema* target = find_schema(table, statement.alter_table.table_name);
    if (target == NULL) {
        return fail_result(result,
                           TINYDB_SQL_ROUTE_ERROR,
                           "ALTER TABLE target table was not found");
    }
    if (ci_equal(target->name, "users") || fixed_row_shape(target)) {
        return fail_result(
            result,
            TINYDB_SQL_POLICY_ERROR,
            "ALTER TABLE ADD COLUMN is disabled for executable fixed-Row table roots until physical row migration is implemented");
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    bool executable_generic = tinydb_schema_supports_records(
        target, schema_message, sizeof(schema_message));
    if (executable_generic &&
        (target->row_size > ROW_SIZE ||
         type.storage_size > ROW_SIZE - target->row_size)) {
        return fail_result(
            result,
            TINYDB_SQL_POLICY_ERROR,
            "ALTER TABLE ADD COLUMN would exceed the fixed generic record slot; variable-size row migration is not implemented");
    }

    if (!table_add_column(table,
                          statement.alter_table.table_name,
                          statement.alter_table.new_col_name,
                          statement.alter_table.new_col_type)) {
        return fail_result(result,
                           TINYDB_SQL_EXECUTE_ERROR,
                           "ALTER TABLE ADD COLUMN failed");
    }

    printf("Column '%s' added to table '%s'.\n",
           statement.alter_table.new_col_name,
           statement.alter_table.table_name);

    db_checkpoint(table);
    if (!multitable_catalog_save(table, database->filename)) {
        return fail_result(result,
                           TINYDB_SQL_CATALOG_PERSIST_ERROR,
                           "schema catalog could not be persisted");
    }

    initialize_result(result, TINYDB_SQL_SUCCESS);
    if (result != NULL) {
        result->executed = true;
        result->schema_persisted = true;
    }
    return TINYDB_SQL_SUCCESS;
}
