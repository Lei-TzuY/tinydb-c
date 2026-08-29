#include "column_type.h"
#include "record.h"
#include "record_payload.h"
#include "table.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool table_add_column_legacy_base(Table* table,
                                  const char* table_name,
                                  const char* col_name,
                                  const char* col_type);

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

static bool wide_schema_table_is_empty(Table* table,
                                       const TableSchema* schema) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    bool scan_complete = false;
    uint32_t row_count = tinydb_record_payload_scan(table,
                                                    schema,
                                                    NULL,
                                                    NULL,
                                                    &scan_complete,
                                                    message,
                                                    sizeof(message));
    return scan_complete && row_count == 0u;
}

bool table_add_column(Table* table,
                      const char* table_name,
                      const char* col_name,
                      const char* col_type) {
    TinyDBColumnTypeSpec type;
    if (!tinydb_column_type_parse(col_type, &type) ||
        type.type != COL_TYPE_VARCHAR ||
        !type.explicitly_sized) {
        return table_add_column_legacy_base(table,
                                            table_name,
                                            col_name,
                                            col_type);
    }

    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL) {
        printf("Error: Table '%s' not found.\n", table_name);
        return false;
    }

    uint32_t old_row_size = schema->row_size;
    uint32_t old_num_columns = schema->num_columns;
    if (old_row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type.storage_size > TINYDB_RECORD_PAYLOAD_MAX - old_row_size) {
        printf("Error: ALTER TABLE ADD COLUMN would exceed the schema-sized payload limit.\n");
        return false;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    bool executable_generic = tinydb_schema_supports_records(
        schema, schema_message, sizeof(schema_message));
    if (executable_generic && old_row_size <= ROW_SIZE &&
        type.storage_size > ROW_SIZE - old_row_size) {
        printf("Error: ALTER TABLE ADD COLUMN would exceed the fixed generic record slot.\n");
        return false;
    }
    if (old_row_size > ROW_SIZE &&
        !wide_schema_table_is_empty(table, schema)) {
        printf("Error: ALTER TABLE ADD COLUMN requires physical row migration for a non-empty schema-sized payload table.\n");
        return false;
    }

    /* The legacy catalog mutator owns duplicate/max-column/users checks. It
     * temporarily treats this as a generic VARCHAR; after success restore the
     * canonical compact n+1 byte physical layout from the shared type parser.
     * Wide schemas reach this point only when a payload scan proved that no
     * physical row image needs migration. */
    if (!table_add_column_legacy_base(table,
                                      table_name,
                                      col_name,
                                      col_type)) {
        return false;
    }

    schema = find_schema(table, table_name);
    if (schema == NULL || schema->num_columns != old_num_columns + 1u) {
        return false;
    }

    TableColumn* added = &schema->columns[old_num_columns];
    added->type = type.type;
    added->offset = old_row_size;
    added->size = type.storage_size;
    schema->row_size = old_row_size + type.storage_size;
    return true;
}
