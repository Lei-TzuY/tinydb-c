#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>

#define GENERIC_SQL_MAX_VALUES MAX_COLUMNS_PER_TABLE

typedef struct {
    const char* current;
} GenericParser;

typedef struct {
    const TableSchema* schema;
    uint32_t seen;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
} GenericSelectContext;

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

static bool is_legacy_fixed_row_schema(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR &&
           schema->row_size == ROW_SIZE;
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
    } else {
        GenericParser parser;
        parser.current = sql;
        if (!consume_word(&parser, "select")) return NULL;

        if (consume_char(&parser, '*')) {
            /* SELECT * */
        } else if (consume_word(&parser, "count")) {
            if (!consume_char(&parser, '(') ||
                !consume_char(&parser, '*') ||
                !consume_char(&parser, ')')) {
                return NULL;
            }
        } else {
            return NULL;
        }
        if (!consume_word(&parser, "from") ||
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
                                    "generic INSERT syntax is INSERT INTO table VALUES (...) ");
    }

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        values[i].type = schema->columns[i].type;
        bool parsed = false;
        if (schema->columns[i].type == COL_TYPE_INT) {
            parsed = parse_uint32(&parser, &values[i].int_value);
        } else {
            parsed = parse_string(&parser, values[i].text, sizeof(values[i].text));
        }
        if (!parsed) {
            return generic_syntax_error(result,
                                        STATEMENT_INSERT,
                                        "generic INSERT value does not match the target column type");
        }
        if (i + 1 < schema->num_columns) {
            if (!consume_char(&parser, ',')) {
                return generic_syntax_error(result,
                                            STATEMENT_INSERT,
                                            "generic INSERT value count does not match the table schema");
            }
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

    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_INSERT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static bool print_scan_record(const TableSchema* schema,
                              const TinyDBRecord* record,
                              void* raw_context) {
    GenericSelectContext* context = (GenericSelectContext*)raw_context;
    if (context->seen < context->offset) {
        context->seen++;
        return true;
    }
    if (context->has_limit &&
        context->seen - context->offset >= context->limit) {
        return false;
    }
    tinydb_record_print(schema, record);
    context->seen++;
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
    bool has_id = false;
    uint32_t id = 0;
    bool has_limit = false;
    uint32_t limit = 0;
    uint32_t offset = 0;

    if (!consume_word(&parser, "select")) {
        return generic_syntax_error(result, STATEMENT_SELECT, "invalid generic SELECT");
    }
    if (consume_char(&parser, '*')) {
        count_only = false;
    } else if (consume_word(&parser, "count")) {
        if (!consume_char(&parser, '(') ||
            !consume_char(&parser, '*') ||
            !consume_char(&parser, ')')) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "generic SELECT currently supports only * or COUNT(*)");
        }
        count_only = true;
    } else {
        return generic_syntax_error(result,
                                    STATEMENT_SELECT,
                                    "generic SELECT currently supports only * or COUNT(*)");
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
        if (!parse_identifier(&parser, column, sizeof(column)) ||
            !ci_equal(column, "id") ||
            !consume_char(&parser, '=') ||
            !parse_uint32(&parser, &id)) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "generic SELECT currently supports only WHERE id = N");
        }
        has_id = true;
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

    if (count_only) {
        uint32_t count = 0;
        if (has_id) {
            TinyDBRecord record;
            count = tinydb_record_find(table, schema, id, &record) ? 1u : 0u;
        } else {
            count = tinydb_record_scan(table, schema, NULL, NULL);
        }
        if (offset > 0) count = 0;
        if (has_limit && limit == 0) count = 0;
        printf("%u\n", count);
    } else if (has_id) {
        if (offset == 0 && (!has_limit || limit > 0)) {
            TinyDBRecord record;
            if (tinydb_record_find(table, schema, id, &record)) {
                tinydb_record_print(schema, &record);
            }
        }
    } else {
        GenericSelectContext context;
        context.schema = schema;
        context.seen = 0;
        context.offset = offset;
        context.limit = limit;
        context.has_limit = has_limit;
        (void)tinydb_record_scan(table, schema, print_scan_record, &context);
    }

    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
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

    if (type == STATEMENT_INSERT) {
        return execute_insert(table, schema, sql, output);
    }
    return execute_select(table, schema, sql, output);
}
