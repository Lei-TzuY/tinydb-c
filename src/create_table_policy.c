#include "engine.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_base(TinyDB* database,
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

static bool is_int_type(const char* type) {
    return ci_equal(type, "INT") || ci_equal(type, "INTEGER");
}

static bool varchar_storage_size(const char* type,
                                 uint32_t* storage_size,
                                 bool* explicitly_sized) {
    if (ci_equal(type, "VARCHAR")) {
        if (storage_size != NULL) *storage_size = 256u;
        if (explicitly_sized != NULL) *explicitly_sized = false;
        return true;
    }
    if (type == NULL) return false;

    static const char prefix[] = "VARCHAR(";
    const char* current = type;
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

static bool legacy_column_names_and_types(const CreateTableStatement* create) {
    uint32_t ignored = 0;
    return create->num_columns == 3u &&
           ci_equal(create->col_names[0], "id") &&
           ci_equal(create->col_names[1], "username") &&
           ci_equal(create->col_names[2], "email") &&
           is_int_type(create->col_types[0]) &&
           varchar_storage_size(create->col_types[1], &ignored, NULL) &&
           varchar_storage_size(create->col_types[2], &ignored, NULL);
}

static bool legacy_fixed_row_shape(const CreateTableStatement* create) {
    if (!legacy_column_names_and_types(create)) return false;

    bool username_sized = false;
    bool email_sized = false;
    uint32_t username_storage = 0;
    uint32_t email_storage = 0;
    if (!varchar_storage_size(create->col_types[1],
                              &username_storage,
                              &username_sized) ||
        !varchar_storage_size(create->col_types[2],
                              &email_storage,
                              &email_sized)) {
        return false;
    }

    /* Historical bare VARCHAR schemas use the legacy Row executor even though
     * their catalog metadata recorded a generic 256-byte VARCHAR width. Keep
     * that compatibility. Explicit widths are safe only when they exactly
     * match the physical Row fields. */
    if (!username_sized && !email_sized) return true;
    return username_sized && email_sized &&
           username_storage == USERNAME_SIZE &&
           email_storage == EMAIL_SIZE;
}

static bool generic_record_candidate(const CreateTableStatement* create) {
    return create->num_columns > 0 &&
           ci_equal(create->col_names[0], "id") &&
           is_int_type(create->col_types[0]) &&
           !legacy_column_names_and_types(create);
}

static bool validate_generic_layout(const CreateTableStatement* create,
                                    char* message,
                                    size_t message_size) {
    uint32_t row_size = 0;
    for (uint32_t i = 0; i < create->num_columns; i++) {
        uint32_t column_size = 0;
        if (is_int_type(create->col_types[i])) {
            column_size = (uint32_t)sizeof(uint32_t);
        } else if (!varchar_storage_size(create->col_types[i],
                                         &column_size,
                                         NULL)) {
            /* Preserve the historical metadata-only behavior for unsupported
             * type names. This wrapper only owns executable generic layouts. */
            return true;
        }

        if (row_size > ROW_SIZE || column_size > ROW_SIZE - row_size) {
            snprintf(message,
                     message_size,
                     "CREATE TABLE row layout exceeds the fixed generic record slot; variable-size/slotted-page rows are not implemented");
            return false;
        }
        row_size += column_size;
    }
    return true;
}

static TinyDBSqlStatus policy_error(TinyDBSqlResult* result,
                                    const char* message) {
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = TINYDB_SQL_POLICY_ERROR;
        result->prepare_result = PREPARE_SUCCESS;
        result->execute_result = EXECUTE_SUCCESS;
        result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
        result->statement_type = STATEMENT_CREATE_TABLE;
        result->statement_type_valid = true;
        result->executed = false;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return TINYDB_SQL_POLICY_ERROR;
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    if (database != NULL && sql != NULL) {
        Statement statement;
        memset(&statement, 0, sizeof(statement));
        if (prepare_statement(sql, &statement) == PREPARE_SUCCESS &&
            statement.type == STATEMENT_CREATE_TABLE) {
            const CreateTableStatement* create = &statement.create_table;
            if (legacy_column_names_and_types(create) &&
                !legacy_fixed_row_shape(create)) {
                return policy_error(
                    result,
                    "sized legacy Row schemas require username VARCHAR(32) and email VARCHAR(255)");
            }

            if (generic_record_candidate(create)) {
                char message[TINYDB_ENGINE_MESSAGE_MAX];
                if (!validate_generic_layout(create,
                                             message,
                                             sizeof(message))) {
                    return policy_error(result, message);
                }
            }
        }
    }

    return tinydb_execute_sql_base(database, sql, result);
}
