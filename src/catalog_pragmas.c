#include "catalog_pragmas.h"

#include <ctype.h>

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static const TableSchema* find_schema(const Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static const char* skip_spaces(const char* input) {
    while (isspace((unsigned char)*input)) input++;
    return input;
}

static bool consume_word(const char** input, const char* word) {
    const char* current = skip_spaces(*input);
    const char* expected = word;
    while (*expected != '\0' &&
           tolower((unsigned char)*current) == tolower((unsigned char)*expected)) {
        current++;
        expected++;
    }
    if (*expected != '\0') return false;
    if (isalnum((unsigned char)*current) || *current == '_') return false;
    *input = current;
    return true;
}

static bool parse_identifier(const char** input, char* output, size_t output_size) {
    const char* current = skip_spaces(*input);
    if (!isalpha((unsigned char)*current) && *current != '_') return false;
    const char* start = current;
    while (isalnum((unsigned char)*current) || *current == '_') current++;
    size_t length = (size_t)(current - start);
    if (length == 0 || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    *input = current;
    return true;
}

static bool extract_target(const char* sql,
                           const char* pragma_name,
                           char* output,
                           size_t output_size) {
    const char* current = sql;
    output[0] = '\0';

    if (!consume_word(&current, "pragma") ||
        !consume_word(&current, pragma_name)) {
        return false;
    }

    current = skip_spaces(current);
    if (*current == ';' || *current == '\0') {
        snprintf(output, output_size, "users");
        return true;
    }
    if (*current != '(') return false;
    current++;
    if (!parse_identifier(&current, output, output_size)) return false;
    current = skip_spaces(current);
    if (*current != ')') return false;
    current++;
    current = skip_spaces(current);
    if (*current == ';') {
        current++;
        current = skip_spaces(current);
    }
    return *current == '\0';
}

static const char* column_type_name(ColumnType type) {
    return type == COL_TYPE_INT ? "INT" : "VARCHAR";
}

static void print_table_info(const TableSchema* schema) {
    printf("cid | name | type | notnull | dflt_value | pk\n");
    printf("----+------+------+---------+------------+---\n");
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        printf("%u | %s | %s | %u | NULL | %u\n",
               i,
               column->name,
               column_type_name(column->type),
               i == 0 ? 1u : 0u,
               i == 0 ? 1u : 0u);
    }
}

static void print_index_list(Table* table, const TableSchema* schema) {
    printf("seq | name                 | unique | origin | partial\n");
    printf("----+----------------------+--------+--------+--------\n");
    uint32_t sequence = 0;

    if (ci_equal(schema->name, "users") && table->username_index_enabled) {
        printf("%u   | idx_users_username   | 0      | c      | 0\n", sequence++);
    }

    for (uint32_t i = 0; i < table->catalog.num_indexes; i++) {
        const SecondaryIndexMeta* index = &table->catalog.indexes[i];
        if (!index->enabled || !ci_equal(index->table_name, schema->name)) continue;
        if (ci_equal(schema->name, "users") &&
            table->username_index_enabled &&
            ci_equal(index->name, "idx_users_username")) {
            continue;
        }
        printf("%u   | %-20s | 0      | c      | 0\n", sequence++, index->name);
    }

    if (sequence == 0) {
        printf("(no indexes found)\n");
    }
}

CatalogPragmaResult tinydb_execute_catalog_pragma(Table* table,
                                                  StatementType type,
                                                  const char* sql,
                                                  char* error_message,
                                                  size_t error_message_size) {
    const char* pragma_name;
    if (type == STATEMENT_PRAGMA_TABLE_INFO) {
        pragma_name = "table_info";
    } else if (type == STATEMENT_PRAGMA_INDEX_LIST) {
        pragma_name = "index_list";
    } else {
        return CATALOG_PRAGMA_NOT_HANDLED;
    }

    char target[MAX_NAME_SIZE];
    if (!extract_target(sql, pragma_name, target, sizeof(target))) {
        if (error_message != NULL && error_message_size > 0) {
            snprintf(error_message, error_message_size, "invalid PRAGMA target");
        }
        return CATALOG_PRAGMA_INVALID_TARGET;
    }

    const TableSchema* schema = find_schema(table, target);
    if (schema == NULL) {
        if (error_message != NULL && error_message_size > 0) {
            snprintf(error_message,
                     error_message_size,
                     "table '%s' not found",
                     target);
        }
        return CATALOG_PRAGMA_TABLE_NOT_FOUND;
    }

    if (type == STATEMENT_PRAGMA_TABLE_INFO) {
        print_table_info(schema);
    } else {
        print_index_list(table, schema);
    }
    return CATALOG_PRAGMA_SUCCESS;
}
