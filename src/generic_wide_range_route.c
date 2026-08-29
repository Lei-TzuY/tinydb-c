#include "generic_predicate.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_predicate_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef enum {
    WIDE_PROJECTION_STAR = 0,
    WIDE_PROJECTION_COUNT,
    WIDE_PROJECTION_COLUMN
} WideProjectionKind;

typedef struct {
    TableSchema* schema;
    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count;
    WideProjectionKind projection_kind;
    uint32_t projection_column_index;
    uint32_t matched;
    uint32_t emitted;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
    bool decode_failed;
} WideCompoundContext;

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
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool legacy_fixed_row_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool parse_projection(TinyDBGenericParser* parser,
                             WideProjectionKind* kind,
                             char* column,
                             size_t column_size) {
    if (column != NULL && column_size > 0u) column[0] = '\0';
    if (tinydb_generic_consume_char(parser, '*')) {
        if (kind != NULL) *kind = WIDE_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        if (kind != NULL) *kind = WIDE_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    if (column == NULL || column_size == 0u ||
        !tinydb_generic_parse_identifier(parser, column, column_size)) {
        return false;
    }
    if (kind != NULL) *kind = WIDE_PROJECTION_COLUMN;
    return true;
}

static bool consume_limit_offset(TinyDBGenericParser* parser,
                                 bool* has_limit,
                                 uint32_t* limit,
                                 uint32_t* offset) {
    uint32_t ignored_limit = 0u;
    uint32_t ignored_offset = 0u;
    if (limit == NULL) limit = &ignored_limit;
    if (offset == NULL) offset = &ignored_offset;
    if (has_limit != NULL) *has_limit = false;
    *limit = 0u;
    *offset = 0u;

    if (tinydb_generic_consume_word(parser, "limit")) {
        if (!tinydb_generic_parse_uint32(parser, limit)) return false;
        if (has_limit != NULL) *has_limit = true;
        if (tinydb_generic_consume_word(parser, "offset") &&
            !tinydb_generic_parse_uint32(parser, offset)) {
            return false;
        }
        return true;
    }
    if (tinydb_generic_consume_word(parser, "offset")) {
        return tinydb_generic_parse_uint32(parser, offset);
    }
    return true;
}

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus result_error(TinyDBGenericSqlResult* result,
                                           TinyDBGenericSqlStatus status,
                                           const char* message) {
    result->status = status;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = status == TINYDB_GENERIC_SQL_EXECUTE_ERROR
        ? EXECUTE_KEY_NOT_FOUND
        : EXECUTE_SUCCESS;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message : "wide compound SELECT failed");
    return status;
}

static TinyDBGenericSqlStatus result_success(TinyDBGenericSqlResult* result) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    result->message[0] = '\0';
    return result->status;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void print_wide_row_values(const TinyDBValue* values,
                                  uint32_t value_count) {
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

static bool compound_matches(const WideCompoundContext* context,
                             const TinyDBValue* values) {
    for (uint32_t i = 0u; i < context->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &context->predicates[i];
        if (!tinydb_generic_predicate_matches(
                predicate, &values[predicate->column_index])) {
            return false;
        }
    }
    return true;
}

static bool visit_compound_payload(const TableSchema* schema,
                                   const TinyDBRecordPayload* payload,
                                   void* raw_context) {
    WideCompoundContext* context = (WideCompoundContext*)raw_context;
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
    if (!compound_matches(context, values)) return true;

    context->matched++;
    if (context->projection_kind == WIDE_PROJECTION_COUNT ||
        context->matched <= context->offset ||
        (context->has_limit && context->emitted >= context->limit)) {
        return true;
    }

    if (context->projection_kind == WIDE_PROJECTION_COLUMN) {
        print_value(&values[context->projection_column_index]);
    } else {
        print_wide_row_values(values, value_count);
    }
    context->emitted++;
    return true;
}

static void intersect_lower(uint32_t candidate, uint32_t* min_id) {
    if (candidate > *min_id) *min_id = candidate;
}

static void intersect_upper(uint32_t candidate, uint32_t* max_id) {
    if (candidate < *max_id) *max_id = candidate;
}

static bool derive_primary_bounds(const WideCompoundContext* context,
                                  uint32_t* min_id,
                                  uint32_t* max_id,
                                  bool* has_bound,
                                  bool* empty) {
    *min_id = 0u;
    *max_id = UINT32_MAX;
    *has_bound = false;
    *empty = false;

    for (uint32_t i = 0u; i < context->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &context->predicates[i];
        if (predicate->column_index != 0u ||
            predicate->value.type != COL_TYPE_INT) {
            continue;
        }
        uint32_t value = predicate->value.int_value;
        switch (predicate->op) {
            case TINYDB_GENERIC_COMPARE_EQ:
                intersect_lower(value, min_id);
                intersect_upper(value, max_id);
                *has_bound = true;
                break;
            case TINYDB_GENERIC_COMPARE_GT:
                if (value == UINT32_MAX) {
                    *empty = true;
                } else {
                    intersect_lower(value + 1u, min_id);
                    *has_bound = true;
                }
                break;
            case TINYDB_GENERIC_COMPARE_GTE:
                intersect_lower(value, min_id);
                *has_bound = true;
                break;
            case TINYDB_GENERIC_COMPARE_LT:
                if (value == 0u) {
                    *empty = true;
                } else {
                    intersect_upper(value - 1u, max_id);
                    *has_bound = true;
                }
                break;
            case TINYDB_GENERIC_COMPARE_LTE:
                intersect_upper(value, max_id);
                *has_bound = true;
                break;
            case TINYDB_GENERIC_COMPARE_LIKE:
                break;
            default:
                return false;
        }
        if (*min_id > *max_id) *empty = true;
    }
    return true;
}

static TinyDBGenericSqlStatus try_wide_compound_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result,
    bool* applicable) {
    *applicable = false;
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    WideProjectionKind projection_kind;
    char projection_column[MAX_NAME_SIZE];
    if (!parse_projection(&parser,
                          &projection_kind,
                          projection_column,
                          sizeof(projection_column)) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        legacy_fixed_row_shape(schema) ||
        !tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    WideCompoundContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.projection_kind = projection_kind;
    if (projection_kind == WIDE_PROJECTION_COLUMN) {
        int index = tinydb_generic_find_column_index(schema, projection_column);
        if (index < 0) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        context.projection_column_index = (uint32_t)index;
    }

    if (!tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &context.predicates[0])) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    context.predicate_count = 1u;
    if (!tinydb_generic_consume_word(&parser, "and")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    *applicable = true;

    do {
        if (context.predicate_count >= MAX_COLUMNS_PER_TABLE ||
            !tinydb_generic_parse_predicate(
                &parser,
                schema,
                &context.predicates[context.predicate_count])) {
            return result_error(
                result,
                TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                "wide compound SELECT requires typed predicates joined by AND");
        }
        context.predicate_count++;
    } while (tinydb_generic_consume_word(&parser, "and"));

    if (!consume_limit_offset(&parser,
                              &context.has_limit,
                              &context.limit,
                              &context.offset) ||
        !tinydb_generic_consume_end(&parser)) {
        return result_error(
            result,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            "wide compound SELECT contains an unsupported trailing clause");
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(schema,
                                                schema_message,
                                                sizeof(schema_message))) {
        return result_error(result,
                            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                            schema_message);
    }

    uint32_t min_id = 0u;
    uint32_t max_id = UINT32_MAX;
    bool has_primary_bound = false;
    bool empty = false;
    if (!derive_primary_bounds(&context,
                               &min_id,
                               &max_id,
                               &has_primary_bound,
                               &empty)) {
        return result_error(result,
                            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                            "unable to derive wide compound primary-key bounds");
    }

    bool scan_complete = true;
    char scan_message[TINYDB_RECORD_MESSAGE_MAX];
    scan_message[0] = '\0';
    if (!empty) {
        if (has_primary_bound) {
            (void)tinydb_record_payload_scan_range(table,
                                                   schema,
                                                   min_id,
                                                   max_id,
                                                   visit_compound_payload,
                                                   &context,
                                                   &scan_complete,
                                                   scan_message,
                                                   sizeof(scan_message));
        } else {
            (void)tinydb_record_payload_scan(table,
                                            schema,
                                            visit_compound_payload,
                                            &context,
                                            &scan_complete,
                                            scan_message,
                                            sizeof(scan_message));
        }
    }
    if (!scan_complete || context.decode_failed) {
        return result_error(
            result,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            scan_message[0] != '\0'
                ? scan_message
                : "unable to decode schema-sized payload during compound SELECT");
    }

    if (context.projection_kind == WIDE_PROJECTION_COUNT) {
        uint32_t count = context.matched;
        if (context.offset > 0u) count = 0u;
        if (context.has_limit && context.limit == 0u) count = 0u;
        printf("%u\n", count);
    }
    return result_success(result);
}

/*
 * The final schema-sized CRUD layer intentionally owns the fast equality
 * SELECT path because it can seek directly through the payload API. Ordered
 * single predicates are implemented one layer deeper by
 * generic_range_select_sql.c. Detect exactly that proven shape here so the
 * equality-only parser cannot turn a valid wide range query into a syntax
 * error before the payload-native range executor sees it.
 */
static bool is_wide_single_range_select(Table* table, const char* sql) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) return false;

    WideProjectionKind ignored_kind;
    char ignored_column[MAX_NAME_SIZE];
    if (!parse_projection(&parser,
                          &ignored_kind,
                          ignored_column,
                          sizeof(ignored_column)) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        legacy_fixed_row_shape(schema) ||
        !tinydb_generic_consume_word(&parser, "where")) {
        return false;
    }

    TinyDBGenericPredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (!tinydb_generic_parse_predicate(&parser, schema, &predicate) ||
        predicate.op == TINYDB_GENERIC_COMPARE_EQ ||
        predicate.op == TINYDB_GENERIC_COMPARE_LIKE) {
        return false;
    }

    if (!consume_limit_offset(&parser, NULL, NULL, NULL)) return false;
    return tinydb_generic_consume_end(&parser);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (table != NULL && sql != NULL) {
        bool compound_applicable = false;
        TinyDBGenericSqlStatus compound_status =
            try_wide_compound_select(table,
                                     sql,
                                     output,
                                     &compound_applicable);
        if (compound_applicable) return compound_status;

        if (is_wide_single_range_select(table, sql)) {
            TinyDBGenericSqlStatus status =
                tinydb_generic_sql_try_execute_predicate_base(table,
                                                              sql,
                                                              output);
            if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;
        }
    }

    return tinydb_generic_sql_try_execute_wide_base(table, sql, output);
}
