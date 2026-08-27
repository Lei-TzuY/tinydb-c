#include "engine.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_base(TinyDB* database,
                                        const char* sql,
                                        TinyDBSqlResult* result);

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

static bool is_int_type(const char* type) {
    return ci_equal(type, "INT") || ci_equal(type, "INTEGER");
}

static bool is_varchar_type(const char* type) {
    return ci_equal(type, "VARCHAR");
}

static bool legacy_fixed_row_shape(const CreateTableStatement* create) {
    return create->num_columns == 3 &&
           ci_equal(create->col_names[0], "id") &&
           ci_equal(create->col_names[1], "username") &&
           ci_equal(create->col_names[2], "email") &&
           is_int_type(create->col_types[0]) &&
           is_varchar_type(create->col_types[1]) &&
           is_varchar_type(create->col_types[2]);
}

static bool generic_record_candidate(const CreateTableStatement* create) {
    return create->num_columns > 0 &&
           ci_equal(create->col_names[0], "id") &&
           is_int_type(create->col_types[0]) &&
           !legacy_fixed_row_shape(create);
}

static bool validate_generic_layout(const CreateTableStatement* create,
                                    char* message,
                                    size_t message_size) {
    uint32_t row_size = 0;
    for (uint32_t i = 0; i < create->num_columns; i++) {
        uint32_t column_size = 0;
        if (is_int_type(create->col_types[i])) {
            column_size = (uint32_t)sizeof(uint32_t);
        } else if (is_varchar_type(create->col_types[i])) {
            /* Keep this policy aligned with the current catalog/record format:
             * generic VARCHAR columns occupy 256 serialized bytes. */
            column_size = 256u;
        } else {
            /* The historical CREATE TABLE path owns metadata-only/unsupported
             * schema semantics. Do not broaden this wrapper into a new type parser. */
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
            statement.type == STATEMENT_CREATE_TABLE &&
            generic_record_candidate(&statement.create_table)) {
            char message[TINYDB_ENGINE_MESSAGE_MAX];
            if (!validate_generic_layout(&statement.create_table,
                                         message,
                                         sizeof(message))) {
                return policy_error(result, message);
            }
        }
    }

    return tinydb_execute_sql_base(database, sql, result);
}
