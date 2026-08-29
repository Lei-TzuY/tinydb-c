#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_diagnostic_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef struct {
    const char* current;
} TinyDBWideSqlParser;

typedef struct {
    const TableSchema* schema;
    bool count_only;
    bool project_column;
    uint32_t projection_column_index;
    bool has_filter;
    uint32_t filter_column_index;
    TinyDBValue filter_value;
    uint32_t matched;
    uint32_t emitted;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
    bool decode_failed;
} TinyDBWideSelectContext;

static int wide_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_ci_char(*left) != wide_ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static void wide_skip_spaces(TinyDBWideSqlParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool wide_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool wide_consume_word(TinyDBWideSqlParser* parser, const char* word) {
    TinyDBWideSqlParser backup = *parser;
    const char* expected = word;
    wide_skip_spaces(parser);
    while (*expected != '\0' &&
           wide_ci_char(*parser->current) == wide_ci_char(*expected)) {
        parser->current++;
        expected++;
    }
    if (*expected != '\0' || wide_identifier_char(*parser->current)) {
        *parser = backup;
        return false;
    }
    return true;
}

static bool wide_consume_char(TinyDBWideSqlParser* parser, char expected) {
    wide_skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool wide_parse_identifier(TinyDBWideSqlParser* parser,
                                  char* output,
                                  size_t output_size) {
    wide_skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }
    const char* start = parser->current;
    while (wide_identifier_char(*parser->current)) parser->current++;
    size_t length = (size_t)(parser->current - start);
    if (length == 0u || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool wide_parse_uint32(TinyDBWideSqlParser* parser, uint32_t* value) {
    wide_skip_spaces(parser);
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

static bool wide_parse_string(TinyDBWideSqlParser* parser,
                              char* output,
                              size_t output_size) {
    wide_skip_spaces(parser);
    if (*parser->current != '\'') return false;
    parser->current++;
    size_t length = 0u;
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
        if (length + 1u >= output_size) return false;
        output[length++] = value;
    }
    return false;
}

static bool wide_consume_end(TinyDBWideSqlParser* parser) {
    wide_skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    wide_skip_spaces(parser);
    return *parser->current == '\0';
}

static int wide_find_column(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return -1;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        if (wide_ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
}

static TableSchema* wide_find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool wide_is_legacy_row_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_ci_equal(schema->columns[0].name, "id") &&
           wide_ci_equal(schema->columns[1].name, "username") &&
           wide_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool wide_schema_owned(const TableSchema* schema) {
    return schema != NULL && schema->row_size > ROW_SIZE &&
           !wide_is_legacy_row_shape(schema);
}

static void wide_initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus wide_error(TinyDBGenericSqlResult* result,
                                         StatementType type,
                                         TinyDBGenericSqlStatus status,
                                         ExecuteResult execute_result,
                                         const char* message) {
    result->status = status;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = execute_result;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message : "wide generic SQL failed");
    return status;
}

static TinyDBGenericSqlStatus wide_success(TinyDBGenericSqlResult* result,
                                           StatementType type) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    result->message[0] = '\0';
    return result->status;
}

static bool wide_parse_value(TinyDBWideSqlParser* parser,
                             const TableColumn* column,
                             TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return wide_parse_uint32(parser, &value->int_value);
    }
    if (column->type != COL_TYPE_VARCHAR) return false;
    return wide_parse_string(parser, value->text, sizeof(value->text));
}

static bool wide_values_equal(const TinyDBValue* left,
                              const TinyDBValue* right) {
    if (left->type != right->type) return false;
    if (left->type == COL_TYPE_INT) return left->int_value == right->int_value;
    return strcmp(left->text, right->text) == 0;
}

static void wide_print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void wide_print_row(const TinyDBValue* values, uint32_t value_count) {
    printf("(");
    for (uint32_t i = 0u; i < value_count; i++) {
        if (i > 0u) printf(", ");
        if (values[i].type == COL_TYPE_INT) {
            printf("%u", values[i].int_value);
        } else {
            printf("%s", values[i].text);
        }
    }
    printf(")\n");
}

static bool wide_select_visit(const TableSchema* schema,
                              const TinyDBRecordPayload* payload,
                              void* raw_context) {
    TinyDBWideSelectContext* context = (TinyDBWideSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_decode_values(schema,
                                             payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message)) ||
        value_count != schema->num_columns) {
        context->decode_failed = true;
        return false;
    }

    if (context->has_filter &&
        !wide_values_equal(&values[context->filter_column_index],
                           &context->filter_value)) {
        return true;
    }

    context->matched++;
    if (context->count_only || context->matched <= context->offset) return true;
    if (context->has_limit && context->emitted >= context->limit) return false;

    if (context->project_column) {
        wide_print_value(&values[context->projection_column_index]);
    } else {
        wide_print_row(values, value_count);
    }
    context->emitted++;
    return !(context->has_limit && context->emitted >= context->limit);
}

static TinyDBGenericSqlStatus wide_execute_insert(Table* table,
                                                  TableSchema* schema,
                                                  const char* sql,
                                                  TinyDBGenericSqlResult* result) {
    TinyDBWideSqlParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    memset(values, 0, sizeof(values));

    if (!wide_consume_word(&parser, "insert") ||
        !wide_consume_word(&parser, "into") ||
        !wide_parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !wide_ci_equal(table_name, schema->name) ||
        !wide_consume_word(&parser, "values") ||
        !wide_consume_char(&parser, '(')) {
        return wide_error(result,
                          STATEMENT_INSERT,
                          TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                          EXECUTE_SUCCESS,
                          "generic INSERT syntax is INSERT INTO table VALUES (...)");
    }

    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        if (!wide_parse_value(&parser, &schema->columns[i], &values[i])) {
            return wide_error(result,
                              STATEMENT_INSERT,
                              TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                              EXECUTE_SUCCESS,
                              "generic INSERT value does not match the target column type");
        }
        if (i + 1u < schema->num_columns && !wide_consume_char(&parser, ',')) {
            return wide_error(result,
                              STATEMENT_INSERT,
                              TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                              EXECUTE_SUCCESS,
                              "generic INSERT value count does not match the table schema");
        }
    }
    if (!wide_consume_char(&parser, ')') || !wide_consume_end(&parser)) {
        return wide_error(result,
                          STATEMENT_INSERT,
                          TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                          EXECUTE_SUCCESS,
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
            : EXECUTE_SUCCESS;
        return wide_error(result,
                          STATEMENT_INSERT,
                          TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                          execute_result,
                          message);
    }
    return wide_success(result, STATEMENT_INSERT);
}

static bool wide_parse_projection(TinyDBWideSqlParser* parser,
                                  const TableSchema* schema,
                                  TinyDBWideSelectContext* context) {
    if (wide_consume_char(parser, '*')) return true;

    TinyDBWideSqlParser backup = *parser;
    if (wide_consume_word(parser, "count") &&
        wide_consume_char(parser, '(') && wide_consume_char(parser, '*') &&
        wide_consume_char(parser, ')')) {
        context->count_only = true;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!wide_parse_identifier(parser, column, sizeof(column))) return false;
    int index = wide_find_column(schema, column);
    if (index < 0) return false;
    context->project_column = true;
    context->projection_column_index = (uint32_t)index;
    return true;
}

static TinyDBGenericSqlStatus wide_execute_select(Table* table,
                                                  TableSchema* schema,
                                                  const char* sql,
                                                  TinyDBGenericSqlResult* result) {
    TinyDBWideSqlParser parser;
    parser.current = sql;
    TinyDBWideSelectContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    char table_name[MAX_NAME_SIZE];

    if (!wide_consume_word(&parser, "select") ||
        !wide_parse_projection(&parser, schema, &context) ||
        !wide_consume_word(&parser, "from") ||
        !wide_parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !wide_ci_equal(table_name, schema->name)) {
        return wide_error(result,
                          STATEMENT_SELECT,
                          TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                          EXECUTE_SUCCESS,
                          "invalid schema-sized generic SELECT");
    }

    if (wide_consume_word(&parser, "where")) {
        char column[MAX_NAME_SIZE];
        if (!wide_parse_identifier(&parser, column, sizeof(column))) {
            return wide_error(result,
                              STATEMENT_SELECT,
                              TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                              EXECUTE_SUCCESS,
                              "generic SELECT WHERE requires a typed equality predicate");
        }
        int index = wide_find_column(schema, column);
        if (index < 0 || !wide_consume_char(&parser, '=') ||
            !wide_parse_value(&parser,
                              &schema->columns[index],
                              &context.filter_value)) {
            return wide_error(result,
                              STATEMENT_SELECT,
                              TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                              EXECUTE_SUCCESS,
                              "generic SELECT WHERE references an unknown column or typed value");
        }
        context.has_filter = true;
        context.filter_column_index = (uint32_t)index;
    }

    if (wide_consume_word(&parser, "limit")) {
        if (!wide_parse_uint32(&parser, &context.limit)) {
            return wide_error(result,
                              STATEMENT_SELECT,
                              TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                              EXECUTE_SUCCESS,
                              "LIMIT requires an integer");
        }
        context.has_limit = true;
        if (wide_consume_word(&parser, "offset") &&
            !wide_parse_uint32(&parser, &context.offset)) {
            return wide_error(result,
                              STATEMENT_SELECT,
                              TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                              EXECUTE_SUCCESS,
                              "OFFSET requires an integer");
        }
    } else if (wide_consume_word(&parser, "offset") &&
               !wide_parse_uint32(&parser, &context.offset)) {
        return wide_error(result,
                          STATEMENT_SELECT,
                          TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                          EXECUTE_SUCCESS,
                          "OFFSET requires an integer");
    }

    if (!wide_consume_end(&parser)) {
        return wide_error(result,
                          STATEMENT_SELECT,
                          TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                          EXECUTE_SUCCESS,
                          "schema-sized generic SELECT contains an unsupported clause");
    }

    bool scan_complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (context.has_filter && context.filter_column_index == 0u) {
        uint32_t key = context.filter_value.int_value;
        (void)tinydb_record_payload_scan_range(table,
                                               schema,
                                               key,
                                               key,
                                               wide_select_visit,
                                               &context,
                                               &scan_complete,
                                               message,
                                               sizeof(message));
    } else {
        (void)tinydb_record_payload_scan(table,
                                        schema,
                                        wide_select_visit,
                                        &context,
                                        &scan_complete,
                                        message,
                                        sizeof(message));
    }

    if (!scan_complete || context.decode_failed) {
        return wide_error(result,
                          STATEMENT_SELECT,
                          TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                          EXECUTE_SUCCESS,
                          message[0] != '\0'
                              ? message
                              : "unable to decode schema-sized row during SELECT");
    }

    if (context.count_only) {
        uint32_t count = context.matched;
        if (context.offset > 0u) count = 0u;
        if (context.has_limit && context.limit == 0u) count = 0u;
        printf("%u\n", count);
    }
    return wide_success(result, STATEMENT_SELECT);
}

static bool wide_parse_target(const char* sql,
                              StatementType* type,
                              char* table_name,
                              size_t table_name_size) {
    TinyDBWideSqlParser parser;
    parser.current = sql;
    if (wide_consume_word(&parser, "insert") &&
        wide_consume_word(&parser, "into") &&
        wide_parse_identifier(&parser, table_name, table_name_size)) {
        *type = STATEMENT_INSERT;
        return true;
    }

    parser.current = sql;
    if (wide_consume_word(&parser, "select")) {
        if (wide_consume_char(&parser, '*')) {
            /* projection consumed */
        } else {
            TinyDBWideSqlParser backup = parser;
            if (!(wide_consume_word(&parser, "count") &&
                  wide_consume_char(&parser, '(') &&
                  wide_consume_char(&parser, '*') &&
                  wide_consume_char(&parser, ')'))) {
                parser = backup;
                char ignored[MAX_NAME_SIZE];
                if (!wide_parse_identifier(&parser,
                                           ignored,
                                           sizeof(ignored))) {
                    return false;
                }
            }
        }
        if (!wide_consume_word(&parser, "from") ||
            !wide_parse_identifier(&parser, table_name, table_name_size)) {
            return false;
        }
        *type = STATEMENT_SELECT;
        return true;
    }
    return false;
}

/*
 * Generic SQL carries a rich status/message pair, while ExecuteResult is a
 * legacy VM enum that has no "validation error" member. Preserve the existing
 * diagnostic normalization for narrow rows, but route schema-sized INSERT and
 * SELECT before the historical TinyDBRecord guard can reject them. This keeps
 * the 293-byte carrier as an explicit compatibility boundary rather than an
 * SQL-level table-size limit.
 */
TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    wide_initialize_result(output);

    if (table != NULL && sql != NULL) {
        StatementType type = STATEMENT_SELECT;
        char table_name[MAX_NAME_SIZE];
        if (wide_parse_target(sql, &type, table_name, sizeof(table_name))) {
            TableSchema* schema = wide_find_schema(table, table_name);
            if (wide_schema_owned(schema)) {
                char message[TINYDB_RECORD_MESSAGE_MAX];
                if (!tinydb_record_payload_schema_supported(schema,
                                                            message,
                                                            sizeof(message))) {
                    return wide_error(output,
                                      type,
                                      TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                                      EXECUTE_SUCCESS,
                                      message);
                }
                if (type == STATEMENT_INSERT) {
                    return wide_execute_insert(table, schema, sql, output);
                }
                return wide_execute_select(table, schema, sql, output);
            }
        }
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_try_execute_diagnostic_base(table, sql, output);

    if (status == TINYDB_GENERIC_SQL_EXECUTE_ERROR &&
        output->statement_type_valid &&
        output->statement_type == STATEMENT_INSERT &&
        output->execute_result == EXECUTE_KEY_NOT_FOUND &&
        output->message[0] != '\0') {
        output->execute_result = EXECUTE_SUCCESS;
    }

    return status;
}
