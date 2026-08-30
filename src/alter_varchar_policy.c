#include "column_type.h"
#include "engine.h"
#include "generic_index_epoch.h"
#include "multitable.h"
#include "record.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool schema_has_column(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return false;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return true;
    }
    return false;
}

static bool test_fail_before_catalog_persist(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_ALTER_BEFORE_CATALOG_PERSIST");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool test_fail_catalog_save(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_ALTER_CATALOG_SAVE");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool fixed_row_shape(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3u &&
           schema->row_size == ROW_SIZE &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR &&
           schema->columns[0].offset == ID_OFFSET &&
           schema->columns[0].size == ID_SIZE &&
           schema->columns[1].offset == USERNAME_OFFSET &&
           schema->columns[1].size == USERNAME_SIZE &&
           schema->columns[2].offset == EMAIL_OFFSET &&
           schema->columns[2].size == EMAIL_SIZE;
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

static bool schema_table_is_provably_empty(Table* table,
                                           const TableSchema* schema,
                                           char* message,
                                           size_t message_size) {
    bool scan_complete = false;
    uint32_t row_count = tinydb_record_payload_scan(table,
                                                    schema,
                                                    NULL,
                                                    NULL,
                                                    &scan_complete,
                                                    message,
                                                    message_size);
    return scan_complete && row_count == 0u;
}

static bool schema_rows_accept_appended_column(
    Table* table,
    const TableSchema* schema,
    const TinyDBColumnTypeSpec* type,
    char* message,
    size_t message_size) {
    if (table == NULL || schema == NULL || type == NULL ||
        schema->num_columns >= MAX_COLUMNS_PER_TABLE ||
        schema->row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type->storage_size > TINYDB_RECORD_PAYLOAD_MAX - schema->row_size) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "prospective schema is outside append-compatible bounds");
        }
        return false;
    }

    TableSchema prospective = *schema;
    TableColumn* added = &prospective.columns[prospective.num_columns];
    memset(added, 0, sizeof(*added));
    added->type = type->type;
    added->offset = schema->row_size;
    added->size = type->storage_size;
    prospective.num_columns++;
    prospective.row_size += type->storage_size;

    bool scan_complete = false;
    (void)tinydb_record_payload_scan(table,
                                     &prospective,
                                     NULL,
                                     NULL,
                                     &scan_complete,
                                     message,
                                     message_size);
    return scan_complete;
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
        !((type.type == COL_TYPE_VARCHAR && type.explicitly_sized) ||
          type.type == COL_TYPE_INT)) {
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
        /* Preserve the base ALTER route's public not-found diagnostic. */
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }
    if (ci_equal(target->name, "users") || fixed_row_shape(target)) {
        return fail_result(
            result,
            TINYDB_SQL_POLICY_ERROR,
            "ALTER TABLE ADD COLUMN is disabled for executable fixed-Row table roots until physical row migration is implemented");
    }

    if (schema_has_column(target, statement.alter_table.new_col_name)) {
        return fail_result(result,
                           TINYDB_SQL_POLICY_ERROR,
                           "ALTER TABLE ADD COLUMN cannot reuse an existing column name");
    }

    if (target->row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type.storage_size > TINYDB_RECORD_PAYLOAD_MAX - target->row_size) {
        return fail_result(
            result,
            TINYDB_SQL_POLICY_ERROR,
            "ALTER TABLE ADD COLUMN would exceed the schema-sized payload limit");
    }

    /*
     * Compact V2 rows carry their field count, logical length, and a schema
     * fingerprint. For append-only evolution the decoder can reconstruct the
     * historical schema from the current schema's unchanged prefix, verify the
     * stored fingerprint, and materialize missing trailing columns as zero/empty
     * defaults. Prove every existing row accepts the prospective schema before
     * changing catalog metadata. Raw/migration-era rows, malformed envelopes,
     * or non-prefix layouts fail the corruption-aware scan and keep ALTER
     * fail-closed.
     */
    if (target->row_size > ROW_SIZE) {
        char scan_message[TINYDB_RECORD_MESSAGE_MAX];
        if (!schema_rows_accept_appended_column(table,
                                                target,
                                                &type,
                                                scan_message,
                                                sizeof(scan_message))) {
            return fail_result(
                result,
                TINYDB_SQL_POLICY_ERROR,
                "ALTER TABLE ADD COLUMN requires append-compatible compact V2 rows for a non-empty schema-sized payload table");
        }
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    bool executable_generic = tinydb_schema_supports_records(
        target, schema_message, sizeof(schema_message));
    if (executable_generic && target->row_size <= ROW_SIZE &&
        type.storage_size > ROW_SIZE - target->row_size) {
        char scan_message[TINYDB_RECORD_MESSAGE_MAX];
        if (!schema_table_is_provably_empty(table,
                                            target,
                                            scan_message,
                                            sizeof(scan_message))) {
            return fail_result(
                result,
                TINYDB_SQL_POLICY_ERROR,
                "ALTER TABLE ADD COLUMN would exceed the fixed generic record slot; variable-size row migration is not implemented");
        }
    }

    /*
     * Generic secondary-index range snapshots are keyed by a durable mutation
     * epoch. Schema evolution is a logical mutation even when existing compact
     * V2 rows do not need to be rewritten: a later rebuild must decode those
     * historical row generations through the new schema and materialize the
     * appended defaults. Invalidate any snapshot for this table before the
     * catalog mutation so no pre-ALTER sidecar can remain current under the new
     * schema. A harmless extra invalidation is preferable to accepting stale
     * candidates; failed DDL may therefore force a later rebuild without
     * changing query results.
     */
    if (!tinydb_generic_index_epoch_before_mutation(table, target)) {
        return fail_result(result,
                           TINYDB_SQL_EXECUTE_ERROR,
                           "ALTER TABLE ADD COLUMN could not invalidate generic index snapshots");
    }

    TableSchema schema_before_alter = *target;
    if (!table_add_column(table,
                          statement.alter_table.table_name,
                          statement.alter_table.new_col_name,
                          statement.alter_table.new_col_type)) {
        return fail_result(result,
                           TINYDB_SQL_EXECUTE_ERROR,
                           "ALTER TABLE ADD COLUMN failed");
    }

    /*
     * Keep an explicit rollback seam between the in-memory catalog mutation and
     * durable catalog publication. The index epoch may have advanced, which is
     * safe: it only forces stale sidecars to rebuild. The authoritative in-memory
     * schema must stay aligned with the durable catalog whenever publication is
     * rejected or fails.
     */
    if (test_fail_before_catalog_persist()) {
        *target = schema_before_alter;
        return fail_result(result,
                           TINYDB_SQL_CATALOG_PERSIST_ERROR,
                           "ALTER TABLE ADD COLUMN interrupted before schema catalog publication");
    }

    db_checkpoint(table);
    if (test_fail_catalog_save() ||
        !multitable_catalog_save(table, database->filename)) {
        *target = schema_before_alter;
        return fail_result(result,
                           TINYDB_SQL_CATALOG_PERSIST_ERROR,
                           "schema catalog could not be persisted");
    }

    printf("Column '%s' added to table '%s'.\n",
           statement.alter_table.new_col_name,
           statement.alter_table.table_name);

    initialize_result(result, TINYDB_SQL_SUCCESS);
    if (result != NULL) {
        result->executed = true;
        result->schema_persisted = true;
    }
    return TINYDB_SQL_SUCCESS;
}
