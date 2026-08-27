#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* generic_sql.c is compiled with its public entry point renamed so this
 * wrapper can extend UPDATE/DELETE while delegating INSERT/SELECT and the
 * established primary-key-compatible behavior to the original executor. */
TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_legacy(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef struct {
    const char* current;
} MutationParser;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} MutationAssignment;

typedef struct {
    const TableSchema* schema;
    uint32_t filter_column_index;
    TinyDBValue filter_value;
    uint32_t* ids;
    size_t count;
    size_t capacity;
    bool failed;
    char message[TINYDB_RECORD_MESSAGE_MAX];
} MutationMatchContext;

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus syntax_error(TinyDBGenericSqlResult* result,
                                           StatementType type,
                                           const char* message) {
    result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

static TinyDBGenericSqlStatus execute_error(TinyDBGenericSqlResult* result,
                                            StatementType type,
                                            const char* message) {
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

static TinyDBGenericSqlStatus success(TinyDBGenericSqlResult* result,
                                      StatementType type) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

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

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static void skip_spaces(MutationParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool consume_word(MutationParser* parser, const char* word) {
    MutationParser backup = *parser;
    const char* expected = word;
    skip_spaces(parser);
    while (*expected != '\0' &&
           ci_char(*parser->current) == ci_char(*expected)) {
        parser->current++;
        expected++;
    }
    if (*expected != '\0' || is_identifier_char(*parser->current)) {
        *parser = backup;
        return false;
    }
    return true;
}

static bool consume_char(MutationParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool consume_end(MutationParser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    skip_spaces(parser);
    return *parser->current == '\0';
}

static bool parse_identifier(MutationParser* parser,
                             char* output,
                             size_t output_size) {
    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }
    const char* start = parser->current;
    while (is_identifier_char(*parser->current)) parser->current++;
    size_t length = (size_t)(parser->current - start);
    if (length == 0 || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool parse_uint32(MutationParser* parser, uint32_t* value) {
    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) return false;
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == parser->current) {
        return false;
    }
    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_string(MutationParser* parser,
                         char* output,
                         size_t output_size) {
    skip_spaces(parser);
    if (*parser->current != '\'') return false;
    parser->current++;
    size_t length = 0;

    while (*parser->current != '\0') {
        char value = *parser->current++;
        if (value == '\'') {
            if (*parser->current == '\'') {
                parser->current++;
                value = '\'';
            } else {
                if (length >= output_size) return false;
                output[length] = '\0';
                return true;
            }
        }
        if (length + 1 >= output_size) return false;
        output[length++] = value;
    }
    return false;
}

static bool parse_value_for_column(MutationParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static int find_column_index(const TableSchema* schema, const char* name) {
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
}

static bool is_legacy_fixed_row_schema(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* resolve_mutation_target(Table* table,
                                            const char* sql,
                                            StatementType* type) {
    MutationParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];

    if (consume_word(&parser, "update")) {
        *type = STATEMENT_UPDATE;
    } else if (consume_word(&parser, "delete")) {
        if (!consume_word(&parser, "from")) return NULL;
        *type = STATEMENT_DELETE;
    } else {
        return NULL;
    }

    if (!parse_identifier(&parser, table_name, sizeof(table_name))) return NULL;
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return NULL;
    return schema;
}

static bool values_equal(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type != right->type) return false;
    if (left->type == COL_TYPE_INT) {
        return left->int_value == right->int_value;
    }
    return strcmp(left->text, right->text) == 0;
}

static bool append_match_id(MutationMatchContext* context, uint32_t id) {
    if (context->count == context->capacity) {
        size_t next_capacity = context->capacity == 0 ? 16u : context->capacity * 2u;
        if (next_capacity < context->capacity ||
            next_capacity > SIZE_MAX / sizeof(*context->ids)) {
            snprintf(context->message,
                     sizeof(context->message),
                     "%s",
                     "generic predicate match set is too large");
            context->failed = true;
            return false;
        }
        uint32_t* grown = (uint32_t*)realloc(
            context->ids, next_capacity * sizeof(*context->ids));
        if (grown == NULL) {
            snprintf(context->message,
                     sizeof(context->message),
                     "%s",
                     "out of memory collecting generic predicate matches");
            context->failed = true;
            return false;
        }
        context->ids = grown;
        context->capacity = next_capacity;
    }
    context->ids[context->count++] = id;
    return true;
}

static bool collect_match(const TableSchema* schema,
                          const TinyDBRecord* record,
                          void* raw_context) {
    MutationMatchContext* context = (MutationMatchContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!tinydb_record_decode(schema,
                              record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &value_count,
                              message,
                              sizeof(message)) ||
        value_count != schema->num_columns) {
        snprintf(context->message,
                 sizeof(context->message),
                 "%s",
                 message[0] != '\0' ? message : "unable to decode generic record");
        context->failed = true;
        return false;
    }

    if (!values_equal(&values[context->filter_column_index],
                      &context->filter_value)) {
        return true;
    }
    return append_match_id(context, values[0].int_value);
}

static bool collect_matching_ids(Table* table,
                                 const TableSchema* schema,
                                 uint32_t filter_column_index,
                                 const TinyDBValue* filter_value,
                                 MutationMatchContext* context) {
    memset(context, 0, sizeof(*context));
    context->schema = schema;
    context->filter_column_index = filter_column_index;
    context->filter_value = *filter_value;
    (void)tinydb_record_scan(table, schema, collect_match, context);
    return !context->failed;
}

static bool decode_current_row(Table* table,
                               const TableSchema* schema,
                               uint32_t id,
                               TinyDBValue* values,
                               uint32_t* value_count,
                               char* message,
                               size_t message_size) {
    TinyDBRecord record;
    if (!tinydb_record_find(table, schema, id, &record)) {
        snprintf(message, message_size, "%s", "primary key not found");
        return false;
    }
    return tinydb_record_decode(schema,
                                &record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                value_count,
                                message,
                                message_size);
}

static bool update_one(Table* table,
                       const TableSchema* schema,
                       uint32_t id,
                       const MutationAssignment* assignments,
                       uint32_t assignment_count,
                       char* message,
                       size_t message_size) {
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    if (!decode_current_row(table,
                            schema,
                            id,
                            values,
                            &value_count,
                            message,
                            message_size)) {
        return false;
    }

    for (uint32_t i = 0; i < assignment_count; i++) {
        values[assignments[i].column_index] = assignments[i].value;
    }
    return tinydb_record_update(table,
                                schema,
                                id,
                                values,
                                value_count,
                                message,
                                message_size);
}

static void begin_atomic_mutation(Table* table, bool* owns_transaction) {
    *owns_transaction = !table->in_transaction;
    if (!*owns_transaction) return;
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
}

static void finish_atomic_mutation(Table* table, bool owns_transaction) {
    if (!owns_transaction) return;
    table->in_transaction = false;
    pager_commit(table->pager);
}

static void rollback_atomic_mutation(Table* table, bool owns_transaction) {
    if (!owns_transaction) return;
    pager_rollback(table->pager);
    table->in_transaction = false;
}

static TinyDBGenericSqlStatus execute_update_predicate(
    Table* table,
    TableSchema* schema,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    MutationParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    MutationAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;

    if (!consume_word(&parser, "update") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !consume_word(&parser, "set")) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE syntax is UPDATE table SET column=value WHERE column=value");
    }

    while (assignment_count < MAX_COLUMNS_PER_TABLE) {
        char column_name[MAX_NAME_SIZE];
        if (!parse_identifier(&parser, column_name, sizeof(column_name))) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE requires a column assignment");
        }
        int column_index = find_column_index(schema, column_name);
        if (column_index < 0) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE references an unknown column");
        }
        if (column_index == 0) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE cannot change the primary-key id");
        }
        if (!consume_char(&parser, '=')) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE assignment requires '='");
        }

        MutationAssignment* assignment = &assignments[assignment_count++];
        assignment->column_index = (uint32_t)column_index;
        if (!parse_value_for_column(&parser,
                                    &schema->columns[column_index],
                                    &assignment->value)) {
            return syntax_error(
                result,
                STATEMENT_UPDATE,
                "generic UPDATE value does not match the target column type");
        }

        if (consume_word(&parser, "where")) break;
        if (!consume_char(&parser, ',')) {
            return syntax_error(
                result,
                STATEMENT_UPDATE,
                "generic UPDATE requires a WHERE equality predicate after assignments");
        }
    }

    if (assignment_count == 0 || assignment_count >= MAX_COLUMNS_PER_TABLE) {
        return syntax_error(result,
                            STATEMENT_UPDATE,
                            "generic UPDATE has too many assignments or no WHERE clause");
    }

    char filter_column[MAX_NAME_SIZE];
    if (!parse_identifier(&parser, filter_column, sizeof(filter_column))) {
        return syntax_error(result,
                            STATEMENT_UPDATE,
                            "generic UPDATE WHERE requires a column name");
    }
    int filter_index = find_column_index(schema, filter_column);
    if (filter_index < 0 || !consume_char(&parser, '=')) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE WHERE references an unknown column or operator");
    }

    TinyDBValue filter_value;
    if (!parse_value_for_column(&parser,
                                &schema->columns[filter_index],
                                &filter_value) ||
        !consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE filter value does not match the target column type");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (filter_index == 0) {
        if (!update_one(table,
                        schema,
                        filter_value.int_value,
                        assignments,
                        assignment_count,
                        message,
                        sizeof(message))) {
            return execute_error(result, STATEMENT_UPDATE, message);
        }
        return success(result, STATEMENT_UPDATE);
    }

    MutationMatchContext matches;
    if (!collect_matching_ids(table,
                              schema,
                              (uint32_t)filter_index,
                              &filter_value,
                              &matches)) {
        TinyDBGenericSqlStatus status = execute_error(
            result,
            STATEMENT_UPDATE,
            matches.message[0] != '\0'
                ? matches.message
                : "unable to collect generic UPDATE matches");
        free(matches.ids);
        return status;
    }

    bool owns_transaction = false;
    begin_atomic_mutation(table, &owns_transaction);
    for (size_t i = 0; i < matches.count; i++) {
        if (!update_one(table,
                        schema,
                        matches.ids[i],
                        assignments,
                        assignment_count,
                        message,
                        sizeof(message))) {
            rollback_atomic_mutation(table, owns_transaction);
            free(matches.ids);
            return execute_error(result, STATEMENT_UPDATE, message);
        }
    }
    finish_atomic_mutation(table, owns_transaction);
    free(matches.ids);
    return success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus execute_delete_predicate(
    Table* table,
    TableSchema* schema,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    MutationParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];

    if (!consume_word(&parser, "delete") ||
        !consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name)) {
        return syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE syntax is DELETE FROM table [WHERE column=value]");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (consume_end(&parser)) {
        (void)tinydb_record_delete_all(table, schema, message, sizeof(message));
        if (message[0] != '\0') {
            return execute_error(result, STATEMENT_DELETE, message);
        }
        return success(result, STATEMENT_DELETE);
    }

    if (!consume_word(&parser, "where")) {
        return syntax_error(result,
                            STATEMENT_DELETE,
                            "generic DELETE requires a WHERE equality predicate");
    }

    char filter_column[MAX_NAME_SIZE];
    if (!parse_identifier(&parser, filter_column, sizeof(filter_column))) {
        return syntax_error(result,
                            STATEMENT_DELETE,
                            "generic DELETE WHERE requires a column name");
    }
    int filter_index = find_column_index(schema, filter_column);
    if (filter_index < 0 || !consume_char(&parser, '=')) {
        return syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE WHERE references an unknown column or operator");
    }

    TinyDBValue filter_value;
    if (!parse_value_for_column(&parser,
                                &schema->columns[filter_index],
                                &filter_value) ||
        !consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE filter value does not match the target column type");
    }

    if (filter_index == 0) {
        if (!tinydb_record_delete(table,
                                  schema,
                                  filter_value.int_value,
                                  message,
                                  sizeof(message))) {
            return execute_error(result, STATEMENT_DELETE, message);
        }
        return success(result, STATEMENT_DELETE);
    }

    MutationMatchContext matches;
    if (!collect_matching_ids(table,
                              schema,
                              (uint32_t)filter_index,
                              &filter_value,
                              &matches)) {
        TinyDBGenericSqlStatus status = execute_error(
            result,
            STATEMENT_DELETE,
            matches.message[0] != '\0'
                ? matches.message
                : "unable to collect generic DELETE matches");
        free(matches.ids);
        return status;
    }

    bool owns_transaction = false;
    begin_atomic_mutation(table, &owns_transaction);
    for (size_t i = 0; i < matches.count; i++) {
        if (!tinydb_record_delete(table,
                                  schema,
                                  matches.ids[i],
                                  message,
                                  sizeof(message))) {
            rollback_atomic_mutation(table, owns_transaction);
            free(matches.ids);
            return execute_error(result, STATEMENT_DELETE, message);
        }
    }
    finish_atomic_mutation(table, owns_transaction);
    free(matches.ids);
    return success(result, STATEMENT_DELETE);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    if (table == NULL || sql == NULL) {
        return tinydb_generic_sql_try_execute_legacy(table, sql, result);
    }

    StatementType type = STATEMENT_SELECT;
    TableSchema* schema = resolve_mutation_target(table, sql, &type);
    if (schema == NULL) {
        return tinydb_generic_sql_try_execute_legacy(table, sql, result);
    }

    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return execute_error(output, type, schema_message);
    }

    if (type == STATEMENT_UPDATE) {
        return execute_update_predicate(table, schema, sql, output);
    }
    return execute_delete_predicate(table, schema, sql, output);
}
