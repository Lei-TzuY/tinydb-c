#include "table.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool table_create_table_legacy_base(Table* table,
                                    const char* name,
                                    uint32_t num_cols,
                                    char col_names[][32],
                                    char col_types[][16],
                                    bool has_fk,
                                    const char* fk_col,
                                    const char* fk_parent_table,
                                    const char* fk_parent_col,
                                    bool fk_on_delete_cascade);

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

static bool parse_varchar_storage(const char* type,
                                  uint32_t* storage_size,
                                  bool* explicitly_sized) {
    if (type == NULL) return false;
    if (ci_equal(type, "VARCHAR")) {
        if (storage_size != NULL) *storage_size = 256u;
        if (explicitly_sized != NULL) *explicitly_sized = false;
        return true;
    }

    const char* current = type;
    static const char prefix[] = "VARCHAR(";
    for (size_t i = 0; i < sizeof(prefix) - 1u; i++) {
        if (ci_char(current[i]) != ci_char(prefix[i])) return false;
    }
    current += sizeof(prefix) - 1u;
    if (!isdigit((unsigned char)*current)) return false;

    uint32_t declared = 0;
    while (isdigit((unsigned char)*current)) {
        declared = declared * 10u + (uint32_t)(*current - '0');
        if (declared > 255u) return false;
        current++;
    }
    if (*current != ')' || current[1] != '\0' || declared == 0u) return false;

    if (storage_size != NULL) *storage_size = declared + 1u;
    if (explicitly_sized != NULL) *explicitly_sized = true;
    return true;
}

static bool is_int_type(const char* type) {
    return ci_equal(type, "INT") || ci_equal(type, "INTEGER");
}

static bool legacy_column_names(uint32_t num_cols, char col_names[][32]) {
    return num_cols == 3u &&
           ci_equal(col_names[0], "id") &&
           ci_equal(col_names[1], "username") &&
           ci_equal(col_names[2], "email");
}

static bool validate_sized_layout(uint32_t num_cols,
                                  char col_names[][32],
                                  char col_types[][16],
                                  uint32_t* sizes,
                                  bool* recognized_all,
                                  bool* has_explicit_width) {
    *recognized_all = true;
    *has_explicit_width = false;

    for (uint32_t i = 0; i < num_cols; i++) {
        if (is_int_type(col_types[i])) {
            sizes[i] = (uint32_t)sizeof(uint32_t);
            continue;
        }

        bool explicitly_sized = false;
        if (parse_varchar_storage(col_types[i], &sizes[i], &explicitly_sized)) {
            if (explicitly_sized) *has_explicit_width = true;
            continue;
        }

        sizes[i] = 0u;
        *recognized_all = false;
    }

    if (*has_explicit_width && legacy_column_names(num_cols, col_names)) {
        if (!is_int_type(col_types[0])) return false;
        uint32_t username_size = 0;
        uint32_t email_size = 0;
        if (!parse_varchar_storage(col_types[1], &username_size, NULL) ||
            !parse_varchar_storage(col_types[2], &email_size, NULL)) {
            return false;
        }
        if (username_size != USERNAME_SIZE || email_size != EMAIL_SIZE) {
            printf("Error: sized legacy Row schemas require username VARCHAR(32) and email VARCHAR(255).\n");
            return false;
        }
    }

    return true;
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

bool table_create_table(Table* table,
                        const char* name,
                        uint32_t num_cols,
                        char col_names[][32],
                        char col_types[][16],
                        bool has_fk,
                        const char* fk_col,
                        const char* fk_parent_table,
                        const char* fk_parent_col,
                        bool fk_on_delete_cascade) {
    if (table == NULL || name == NULL || num_cols == 0u ||
        num_cols > MAX_COLUMNS_PER_TABLE) {
        return false;
    }

    uint32_t sizes[MAX_COLUMNS_PER_TABLE] = {0};
    bool recognized_all = false;
    bool has_explicit_width = false;
    if (!validate_sized_layout(num_cols,
                               col_names,
                               col_types,
                               sizes,
                               &recognized_all,
                               &has_explicit_width)) {
        return false;
    }

    if (recognized_all && has_explicit_width &&
        ci_equal(col_names[0], "id") && is_int_type(col_types[0]) &&
        !legacy_column_names(num_cols, col_names)) {
        uint32_t row_size = 0;
        for (uint32_t i = 0; i < num_cols; i++) {
            if (row_size > ROW_SIZE || sizes[i] > ROW_SIZE - row_size) {
                printf("Error: CREATE TABLE row layout exceeds the fixed generic record slot.\n");
                return false;
            }
            row_size += sizes[i];
        }
    }

    if (!table_create_table_legacy_base(table,
                                        name,
                                        num_cols,
                                        col_names,
                                        col_types,
                                        has_fk,
                                        fk_col,
                                        fk_parent_table,
                                        fk_parent_col,
                                        fk_on_delete_cascade)) {
        return false;
    }

    if (!has_explicit_width) return true;

    TableSchema* schema = find_schema(table, name);
    if (schema == NULL) return false;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < num_cols; i++) {
        if (is_int_type(col_types[i])) {
            schema->columns[i].type = COL_TYPE_INT;
            schema->columns[i].size = (uint32_t)sizeof(uint32_t);
        } else if (parse_varchar_storage(col_types[i],
                                         &schema->columns[i].size,
                                         NULL)) {
            schema->columns[i].type = COL_TYPE_VARCHAR;
        } else {
            /* Preserve the historical fallback for metadata-only unsupported
             * type names if a mixed schema ever reaches this internal API. */
        }
        schema->columns[i].offset = offset;
        offset += schema->columns[i].size;
    }
    schema->row_size = offset;
    return true;
}
