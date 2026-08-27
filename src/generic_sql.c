#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>

#define GENERIC_SQL_MAX_VALUES MAX_COLUMNS_PER_TABLE

typedef struct {
    const char* current;
} GenericParser;

typedef struct {
    const TableSchema* schema;
    bool has_filter;
    uint32_t filter_column_index;
    TinyDBValue filter_value;
    bool project_column;
    uint32_t projection_column_index;
    uint32_t matched;
    uint32_t emitted;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
    bool count_only;
} GenericSelectContext;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} GenericAssignment;

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static void set_message(TinyDBGenericSqlResult* result, const char* message) {
    snprintf(result->message, sizeof(result->message), "%s", message);
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

static void skip_spaces(GenericParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_word(GenericParser* parser, const char* word) {
    GenericParser backup = *parser;
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

static bool consume_char(GenericParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool parse_identifier(GenericParser* parser,
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

static bool parse_uint32(GenericParser* parser, uint32_t* value) {
    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) return false;
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == parser->current) return false;
    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_string(GenericParser* parser, char* output, size_t output_size) {
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

static bool consume_end(GenericParser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    skip_spaces(parser);
    return *parser->current == '\0';
}

static TableSchema* find_schema_exact(Table* table, const char* name) {
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
    /* Keep the exact compatibility predicate used by multitable.c. Older
     * CREATE TABLE metadata records VARCHAR columns as 256 bytes each even
     * though the legacy physical Row slot is 293 bytes, so row_size itself
     * cannot be used to decide which execution path owns the table. */
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool parse_target_after_prefix(const char* sql,
                                      const char* first,
                                      const char* second,
                                      char* table_name,
                                      size_t table_name_size) {
    GenericParser parser;
    parser.current = sql;
    if (!consume_word(&parser, first)) return false;
    if (second != NULL && !consume_word(&parser, second)) return false;
    return parse_identifier(&parser, table_name, table_name_size);
}

static bool parse_select_projection_for_routing(GenericParser* parser) {
    if (consume_char(parser, '*')) return true;

    GenericParser backup = *parser;
    if (consume_word(parser, "count") &&
        consume_char(parser, '(') &&
        consume_char(parser, '*') &&
        consume_char(parser, ')')) {
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    return parse_identifier(parser, column, sizeof(column));
}

static TableSchema* resolve_generic_target(Table* table,
                                           const char* sql,
                                           StatementType* type) {
    char table_name[MAX_NAME_SIZE];
    if (parse_target_after_prefix(sql,
                                  "insert",
                                  "into",
                                  table_name,
                                  sizeof(table_name))) {
        *type = STATEMENT_INSERT;
    } else if (parse_target_after_prefix(sql,
                                         "update",
                                         NULL,
                                         table_name,
                                         sizeof(table_name))) {
        *type = STATEMENT_UPDATE;
    } else if (parse_target_after_prefix(sql,
                                         "delete",
                                         "from",
                                         table_name,
                                         sizeof(table_name))) {
        *type = STATEMENT_DELETE;
    } else {
        GenericParser parser;
        parser.current = sql;
        if (!consume_word(&parser, "select") ||
            !parse_select_projection_for_routing(&parser) ||
            !consume_word(&parser, "from") ||
            !parse_identifier(&parser, table_name, sizeof(table_name))) {
            return NULL;
        }
        *type = STATEMENT_SELECT;
    }

    TableSchema* schema = find_schema_exact(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return NULL;
    return schema;
}

static TinyDBGenericSqlStatus generic_syntax_error(TinyDBGenericSqlResult* result,
                                                    StatementType type,
                                                    const char* message) {
    result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    set_message(result, message);
    return result->status;
}

static TinyDBGenericSqlStatus generic_execute_error(TinyDBGenericSqlResult* result,
                                                     StatementType type,
                                                     ExecuteResult execute_result,
                                                     const char* message) {
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = execute_result;
    set_message(result, message);
    return result->status;
}

static TinyDBGenericSqlStatus generic_success(TinyDBGenericSqlResult* result,
                                              StatementType type) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static bool parse_value_for_column(GenericParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static TinyDBGenericSqlStatus execute_insert(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    TinyDBValue values[GENERIC_SQL_MAX_VALUES];
    memset(values, 0, sizeof(values));

    if (!consume_word(&parser, "insert") ||
        !consume_word(&parser, "into") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !consume_word(&parser, "values") ||
        !consume_char(&parser, '(')) {
        return generic_syntax_error(result,
                                    STATEMENT_INSERT,
                                    "generic INSERT syntax is INSERT INTO table VALUES (...)");
    }

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (!parse_value_for_column(&parser, &schema->columns[i], &values[i])) {
            return generic_syntax_error(result,
                                        STATEMENT_INSERT,
                                        "generic INSERT value does not match the target column type");
        }
        if (i + 1 < schema->num_columns && !consume_char(&parser, ',')) {
            return generic_syntax_error(result,
                                        STATEMENT_INSERT,
                                        "generic INSERT value count does not match the table schema");
        }
    }

    if (!consume_char(&parser, ')') || !consume_end(&parser)) {
        return generic_syntax_error(result,
                                    STATEMENT_INSERT,
                                    "generic INSERT has trailing or extra values");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_insert(table,
                              schema,
                              values,
                              schema->num_columns,
                              message,
                              sizeof(message))) {
        ExecuteResult execute_result = strcmp(message, "duplicate primary key") == 0
            ? EXECUTE_DUPLICATE_KEY
            : EXECUTE_KEY_NOT_FOUND;
        return generic_execute_error(result,
                                     STATEMENT_INSERT,
                                     execute_result,
                                     message);
    }

    return generic_success(result, STATEMENT_INSERT);
}

static TinyDBGenericSqlStatus execute_update(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    GenericAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;

    if (!consume_word(&parser, "update") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !consume_word(&parser, "set")) {
        return generic_syntax_error(result,
                                    STATEMENT_UPDATE,
                                    "generic UPDATE syntax is UPDATE table SET column=value WHERE id=N");
    }

    while (assignment_count < MAX_COLUMNS_PER_TABLE) {
        char column_name[MAX_NAME_SIZE];
        if (!parse_identifier(&parser, column_name, sizeof(column_name))) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE requires a column assignment");
        }
        int column_index = find_column_index(schema, column_name);
        if (column_index < 0) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE references an unknown column");
        }
        if (column_index == 0) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE cannot change the primary-key id");
        }
        if (!consume_char(&parser, '=')) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE assignment requires '='");
        }

        GenericAssignment* assignment = &assignments[assignment_count++];
        assignment->column_index = (uint32_t)column_index;
        if (!parse_value_for_column(&parser,
                                    &schema->columns[column_index],
                                    &assignment->value)) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE value does not match the target column type");
        }

        if (consume_word(&parser, "where")) break;
        if (!consume_char(&parser, ',')) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE requires WHERE id=N after assignments");
        }
    }

    char where_column[MAX_NAME_SIZE];
    uint32_t id = 0;
    if (assignment_count == 0 ||
        !parse_identifier(&parser, where_column, sizeof(where_column)) ||
        !ci_equal(where_column, "id") ||
        !consume_char(&parser, '=') ||
        !parse_uint32(&parser, &id) ||
        !consume_end(&parser)) {
        return generic_syntax_error(result,
                                    STATEMENT_UPDATE,
                                    "generic UPDATE currently requires WHERE id = N");
    }

    TinyDBRecord existing;
    if (!tinydb_record_find(table, schema, id, &existing)) {
        return generic_execute_error(result,
                                     STATEMENT_UPDATE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     "primary key not found");
    }

    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_decode(schema,
                              &existing,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &value_count,
                              message,
                              sizeof(message))) {
        return generic_execute_error(result,
                                     STATEMENT_UPDATE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     message);
    }

    for (uint32_t i = 0; i < assignment_count; i++) {
        values[assignments[i].column_index] = assignments[i].value;
    }

    if (!tinydb_record_update(table,
                              schema,
                              id,
                              values,
                              value_count,
                              message,
                              sizeof(message))) {
        return generic_execute_error(result,
                                     STATEMENT_UPDATE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     message);
    }
    return generic_success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus execute_delete(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];

    if (!consume_word(&parser, "delete") ||
        !consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name)) {
        return generic_syntax_error(result,
                                    STATEMENT_DELETE,
                                    "generic DELETE syntax is DELETE FROM table [WHERE id=N]");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (consume_end(&parser)) {
        (void)tinydb_record_delete_all(table, schema, message, sizeof(message));
        if (message[0] != '\0') {
            return generic_execute_error(result,
                                         STATEMENT_DELETE,
                                         EXECUTE_KEY_NOT_FOUND,
                                         message);
        }
        return generic_success(result, STATEMENT_DELETE);
    }

    char where_column[MAX_NAME_SIZE];
    uint32_t id = 0;
    if (!consume_word(&parser, "where") ||
        !parse_identifier(&parser, where_column, sizeof(where_column)) ||
        !ci_equal(where_column, "id") ||
        !consume_char(&parser, '=') ||
        !parse_uint32(&parser, &id) ||
        !consume_end(&parser)) {
        return generic_syntax_error(result,
                                    STATEMENT_DELETE,
                                    "generic DELETE currently supports only WHERE id = N");
    }

    if (!tinydb_record_delete(table, schema, id, message, sizeof(message))) {
        return generic_execute_error(result,
                                     STATEMENT_DELETE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     message);
    }
    return generic_success(result, STATEMENT_DELETE);
}

static bool values_equal(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type != right->type) return false;
    if (left->type == COL_TYPE_INT) {
        return left->int_value == right->int_value;
    }
    return strcmp(left->text, right->text) == 0;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static bool decode_record_values(const TableSchema* schema,
                                 const TinyDBRecord* record,
                                 TinyDBValue* values) {
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_decode(schema,
                                record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                &value_count,
                                message,
                                sizeof(message)) &&
           value_count == schema->num_columns;
}

static bool record_matches_filter(const GenericSelectContext* context,
                                  const TinyDBValue* values) {
    if (!context->has_filter) return true;
    return values_equal(&values[context->filter_column_index],
                        &context->filter_value);
}

static void print_selected_record(const GenericSelectContext* context,
                                  const TinyDBRecord* record,
                                  const TinyDBValue* values) {
    if (context->project_column) {
        print_value(&values[context->projection_column_index]);
    } else {
        tinydb_record_print(context->schema, record);
    }
}

static bool scan_select_record(const TableSchema* schema,
                               const TinyDBRecord* record,
                               void* raw_context) {
    GenericSelectContext* context = (GenericSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_record_values(schema, record, values)) return false;
    if (!record_matches_filter(context, values)) return true;

    context->matched++;
    if (context->count_only) return true;
    if (context->matched <= context->offset) return true;
    if (context->has_limit && context->emitted >= context->limit) return false;

    print_selected_record(context, record, values);
    context->emitted++;
    return true;
}

static bool parse_select_projection(GenericParser* parser,
                                    const TableSchema* schema,
                                    bool* count_only,
                                    bool* project_column,
                                    uint32_t* projection_column_index) {
    if (consume_char(parser, '*')) {
        *count_only = false;
        *project_column = false;
        *projection_column_index = 0;
        return true;
    }

    GenericParser backup = *parser;
    if (consume_word(parser, "count") &&
        consume_char(parser, '(') &&
        consume_char(parser, '*') &&
        consume_char(parser, ')')) {
        *count_only = true;
        *project_column = false;
        *projection_column_index = 0;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0) return false;
    *count_only = false;
    *project_column = true;
    *projection_column_index = (uint32_t)column_index;
    return true;
}

static TinyDBGenericSqlStatus execute_select(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    bool count_only = false;
    bool project_column = false;
    uint32_t projection_column_index = 0;
    bool has_filter = false;
    uint32_t filter_column_index = 0;
    TinyDBValue filter_value;
    memset(&filter_value, 0, sizeof(filter_value));
    bool has_limit = false;
    uint32_t limit = 0;
    uint32_t offset = 0;

    if (!consume_word(&parser, "select")) {
        return generic_syntax_error(result, STATEMENT_SELECT, "invalid generic SELECT");
    }
    if (!parse_select_projection(&parser,
                                 schema,
                                 &count_only,
                                 &project_column,
                                 &projection_column_index)) {
        return generic_syntax_error(result,
                                    STATEMENT_SELECT,
                                    "generic SELECT projection references an unknown or unsupported column");
    }

    if (!consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name)) {
        return generic_syntax_error(result,
                                    STATEMENT_SELECT,
                                    "generic SELECT requires a catalog-backed table target");
    }

    if (consume_word(&parser, "where")) {
        char column[MAX_NAME_SIZE];
        if (!parse_identifier(&parser, column, sizeof(column))) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "generic SELECT WHERE requires a column name");
        }
        int column_index = find_column_index(schema, column);
        if (column_index < 0 || !consume_char(&parser, '=')) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "generic SELECT WHERE references an unknown column or operator");
        }
        filter_column_index = (uint32_t)column_index;
        if (!parse_value_for_column(&parser,
                                    &schema->columns[filter_column_index],
                                    &filter_value)) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "generic SELECT filter value does not match the target column type");
        }
        has_filter = true;
    }

    if (consume_word(&parser, "limit")) {
        if (!parse_uint32(&parser, &limit)) {
            return generic_syntax_error(result, STATEMENT_SELECT, "LIMIT requires an integer");
        }
        has_limit = true;
        if (consume_word(&parser, "offset")) {
            if (!parse_uint32(&parser, &offset)) {
                return generic_syntax_error(result, STATEMENT_SELECT, "OFFSET requires an integer");
            }
        }
    } else if (consume_word(&parser, "offset")) {
        if (!parse_uint32(&parser, &offset)) {
            return generic_syntax_error(result, STATEMENT_SELECT, "OFFSET requires an integer");
        }
    }

    if (!consume_end(&parser)) {
        return generic_syntax_error(result,
                                    STATEMENT_SELECT,
                                    "generic SELECT contains an unsupported clause");
    }

    GenericSelectContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.has_filter = has_filter;
    context.filter_column_index = filter_column_index;
    context.filter_value = filter_value;
    context.project_column = project_column;
    context.projection_column_index = projection_column_index;
    context.offset = offset;
    context.limit = limit;
    context.has_limit = has_limit;
    context.count_only = count_only;

    /* Preserve the direct B+ tree primary-key path whenever WHERE targets id.
     * All other schema-aware equality predicates use a decoded record scan. */
    if (has_filter && filter_column_index == 0) {
        TinyDBRecord record;
        bool found = tinydb_record_find(table,
                                        schema,
                                        filter_value.int_value,
                                        &record);
        if (found) {
            TinyDBValue values[MAX_COLUMNS_PER_TABLE];
            if (!decode_record_values(schema, &record, values)) {
                return generic_execute_error(result,
                                             STATEMENT_SELECT,
                                             EXECUTE_KEY_NOT_FOUND,
                                             "unable to decode generic record");
            }
            context.matched = 1;
            if (!count_only && offset == 0 && (!has_limit || limit > 0)) {
                print_selected_record(&context, &record, values);
                context.emitted = 1;
            }
        }
    } else {
        (void)tinydb_record_scan(table, schema, scan_select_record, &context);
    }

    if (count_only) {
        uint32_t count = context.matched;
        if (offset > 0) count = 0;
        if (has_limit && limit == 0) count = 0;
        printf("%u\n", count);
    }

    return generic_success(result, STATEMENT_SELECT);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(Table* table,
                                                       const char* sql,
                                                       TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (table == NULL || sql == NULL) {
        return output->status;
    }

    StatementType type = STATEMENT_SELECT;
    TableSchema* schema = resolve_generic_target(table, sql, &type);
    if (schema == NULL) return output->status;

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return generic_execute_error(output,
                                     type,
                                     EXECUTE_KEY_NOT_FOUND,
                                     schema_message);
    }

    switch (type) {
        case STATEMENT_INSERT:
            return execute_insert(table, schema, sql, output);
        case STATEMENT_UPDATE:
            return execute_update(table, schema, sql, output);
        case STATEMENT_DELETE:
            return execute_delete(table, schema, sql, output);
        case STATEMENT_SELECT:
            return execute_select(table, schema, sql, output);
        default:
            return output->status;
    }
}
