#include "record.h"
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

static bool parse_sized_varchar(const char* type, uint32_t* storage_size) {
    if (type == NULL) return false;
    static const char prefix[] = "VARCHAR(";
    for (size_t i = 0; i < sizeof(prefix) - 1u; i++) {
        if (type[i] == '\0' || ci_char(type[i]) != ci_char(prefix[i])) return false;
    }

    const char* current = type + sizeof(prefix) - 1u;
    if (!isdigit((unsigned char)*current)) return false;
    uint32_t declared = 0;
    while (isdigit((unsigned char)*current)) {
        declared = declared * 10u + (uint32_t)(*current - '0');
        if (declared > 255u) return false;
        current++;
    }
    if (*current != ')' || current[1] != '\0' || declared == 0u) return false;
    *storage_size = declared + 1u;
    return true;
}

bool table_add_column(Table* table,
                      const char* table_name,
                      const char* col_name,
                      const char* col_type) {
    uint32_t storage_size = 0;
    if (!parse_sized_varchar(col_type, &storage_size)) {
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
    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    bool executable_generic = tinydb_schema_supports_records(
        schema, schema_message, sizeof(schema_message));
    if (executable_generic &&
        (old_row_size > ROW_SIZE || storage_size > ROW_SIZE - old_row_size)) {
        printf("Error: ALTER TABLE ADD COLUMN would exceed the fixed generic record slot.\n");
        return false;
    }

    /* The legacy catalog mutator owns duplicate/max-column/users checks. It
     * sees this type as VARCHAR and temporarily reserves 256 bytes; after it
     * succeeds, restore the canonical compact n+1 byte physical layout. */
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
    added->type = COL_TYPE_VARCHAR;
    added->offset = old_row_size;
    added->size = storage_size;
    schema->row_size = old_row_size + storage_size;
    return true;
}
