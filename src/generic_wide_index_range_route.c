#include "generic_boolean.h"
#include "generic_index_candidates.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef enum {
    WIDE_INDEX_RANGE_PROJECTION_STAR = 0,
    WIDE_INDEX_RANGE_PROJECTION_COUNT,
    WIDE_INDEX_RANGE_PROJECTION_COLUMN
} WideIndexRangeProjectionKind;

typedef struct {
    TableSchema* schema;
    GenericSecondaryIndex* index;
    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count;
    uint32_t indexed_column;
    WideIndexRangeProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} WideIndexRangeSelect;

static int wide_index_range_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_index_range_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_index_range_ci_char(*left) != wide_index_range_ci_char(*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool wide_index_range_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_index_range_ci_equal(schema->columns[0].name, "id") &&
           wide_index_range_ci_equal(schema->columns[1].name, "username") &&
           wide_index_range_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool wide_index_range_schema_owned(const TableSchema* schema) {
    return schema != NULL && schema->row_size > ROW_SIZE &&
           !wide_index_range_legacy_shape(schema);
}

static TableSchema* wide_index_range_find_schema(Table* table,
                                                 const char* table_name) {
    if (table == NULL || table_name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_index_range_ci_equal(table->catalog.schemas[i].name,
                                      table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static GenericSecondaryIndex* wide_index_range_find_index(
    Table* table,
    const TableSchema* schema,
    uint32_t column_index) {
    if (table == NULL || schema == NULL || column_index == 0u ||
        column_index >= schema->num_columns) {
        return NULL;
    }
    for (uint32_t i = 0u; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1u &&
            wide_index_range_ci_equal(index->table_name, schema->name) &&
            wide_index_range_ci_equal(index->column_name,
                                      schema->columns[column_index].name)) {
            return index;
        }
    }
    return NULL;
}

static bool wide_index_range_parse_projection(
    TinyDBGenericParser* parser,
    const TableSchema* schema,
    WideIndexRangeSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = WIDE_INDEX_RANGE_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = WIDE_INDEX_RANGE_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser,
                                         column_name,
                                         sizeof(column_name))) {
        return false;
    }
    int column_index = tinydb_generic_find_column_index(schema, column_name);
    if (column_index < 0) return false;
    select->projection_kind = WIDE_INDEX_RANGE_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool wide_index_range_parse_select(Table* table,
                                          const char* sql,
                                          WideIndexRangeSelect* select) {
    memset(select, 0, sizeof(*select));
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) return false;

    TinyDBGenericParser projection_start = parser;
    if (tinydb_generic_consume_char(&parser, '*')) {
        /* projection consumed only to discover the target schema */
    } else {
        TinyDBGenericParser backup = parser;
        if (!(tinydb_generic_consume_word(&parser, "count") &&
              tinydb_generic_consume_char(&parser, '(') &&
              tinydb_generic_consume_char(&parser, '*') &&
              tinydb_generic_consume_char(&parser, ')'))) {
            parser = backup;
            char ignored[MAX_NAME_SIZE];
            if (!tinydb_generic_parse_identifier(&parser,
                                                 ignored,
                                                 sizeof(ignored))) {
                return false;
            }
        }
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = wide_index_range_find_schema(table, table_name);
    if (!wide_index_range_schema_owned(schema)) return false;
    select->schema = schema;

    parser = projection_start;
    if (!wide_index_range_parse_projection(&parser, schema, select) ||
        !tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name)) ||
        !wide_index_range_ci_equal(table_name, schema->name) ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &select->predicates[0])) {
        return false;
    }
    select->predicate_count = 1u;
    while (tinydb_generic_consume_word(&parser, "and")) {
        if (select->predicate_count >= MAX_COLUMNS_PER_TABLE ||
            !tinydb_generic_parse_predicate(
                &parser,
                schema,
                &select->predicates[select->predicate_count])) {
            return false;
        }
        select->predicate_count++;
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset") &&
            !tinydb_generic_parse_uint32(&parser, &select->offset)) {
            return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }
    if (!tinydb_generic_consume_end(&parser)) return false;

    for (uint32_t i = 0u; i < select->predicate_count; i++) {
        TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == 0u ||
            predicate->op == TINYDB_GENERIC_COMPARE_LIKE) {
            continue;
        }
        GenericSecondaryIndex* index = wide_index_range_find_index(
            table, schema, predicate->column_index);
        if (index != NULL) {
            select->index = index;
            select->indexed_column = predicate->column_index;
            break;
        }
    }
    if (select->index == NULL) return false;

    /* Keep the established single equality route in the lower layer. This
     * adapter owns only the additional range/compound shapes. */
    return select->predicate_count > 1u ||
           select->predicates[0].op != TINYDB_GENERIC_COMPARE_EQ;
}

static bool wide_index_range_predicates_match(
    const WideIndexRangeSelect* select,
    const TinyDBValue* values) {
    for (uint32_t i = 0u; i < select->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (!tinydb_generic_predicate_matches(
                predicate, &values[predicate->column_index])) {
            return false;
        }
    }
    return true;
}

static void wide_index_range_print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void wide_index_range_print_row(const TinyDBValue* values,
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

static TinyDBGenericSqlStatus wide_index_range_error(
    TinyDBGenericSqlResult* result,
    const char* message) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message
                             : "wide indexed range execution failed");
    return result->status;
}

static TinyDBGenericSqlStatus wide_index_range_success(
    TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static bool wide_index_range_collect_candidates(
    Table* table,
    const WideIndexRangeSelect* select,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size) {
    TinyDBGenericPredicate indexed[MAX_COLUMNS_PER_TABLE];
    uint32_t indexed_count = 0u;
    for (uint32_t i = 0u; i < select->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == select->indexed_column &&
            predicate->op != TINYDB_GENERIC_COMPARE_LIKE) {
            indexed[indexed_count++] = *predicate;
        }
    }
    if (indexed_count == 0u) return false;
    if (indexed_count == 1u) {
        return tinydb_generic_index_collect_candidates(table,
                                                       select->schema,
                                                       select->index,
                                                       &indexed[0],
                                                       candidates,
                                                       message,
                                                       message_size);
    }
    return tinydb_generic_index_collect_conjunctive_candidates(
        table,
        select->schema,
        select->index,
        indexed,
        indexed_count,
        candidates,
        message,
        message_size);
}

static TinyDBGenericSqlStatus wide_index_range_execute(
    Table* table,
    const WideIndexRangeSelect* select,
    TinyDBGenericSqlResult* result) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (!tinydb_record_payload_schema_supported(select->schema,
                                                message,
                                                sizeof(message))) {
        return wide_index_range_error(result, message);
    }

    TinyDBGenericIndexCandidates candidates;
    memset(&candidates, 0, sizeof(candidates));
    if (!wide_index_range_collect_candidates(table,
                                             select,
                                             &candidates,
                                             message,
                                             sizeof(message))) {
        return wide_index_range_error(
            result,
            message[0] != '\0'
                ? message
                : "unable to collect schema-sized indexed range candidates");
    }

    uint32_t matched = 0u;
    uint32_t emitted = 0u;
    for (uint32_t i = 0u; i < candidates.count; i++) {
        TinyDBRecordPayload payload;
        message[0] = '\0';
        if (!tinydb_record_payload_find(table,
                                        select->schema,
                                        candidates.ids[i],
                                        &payload,
                                        message,
                                        sizeof(message))) {
            /* Candidate snapshots are hints. Missing rows are stale false
             * positives and must never become fabricated results. */
            continue;
        }

        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0u;
        if (!tinydb_record_payload_decode_values(select->schema,
                                                 &payload,
                                                 values,
                                                 MAX_COLUMNS_PER_TABLE,
                                                 &value_count,
                                                 message,
                                                 sizeof(message)) ||
            value_count != select->schema->num_columns) {
            tinydb_generic_index_candidates_free(&candidates);
            return wide_index_range_error(
                result,
                message[0] != '\0'
                    ? message
                    : "unable to decode schema-sized indexed range candidate");
        }
        if (!wide_index_range_predicates_match(select, values)) continue;

        matched++;
        if (select->projection_kind == WIDE_INDEX_RANGE_PROJECTION_COUNT ||
            matched <= select->offset) {
            continue;
        }
        if (select->has_limit && emitted >= select->limit) break;

        if (select->projection_kind == WIDE_INDEX_RANGE_PROJECTION_COLUMN) {
            wide_index_range_print_value(
                &values[select->projection_column_index]);
        } else {
            wide_index_range_print_row(values, value_count);
        }
        emitted++;
    }

    tinydb_generic_index_candidates_free(&candidates);
    if (select->projection_kind == WIDE_INDEX_RANGE_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0u) count = 0u;
        if (select->has_limit && select->limit == 0u) count = 0u;
        printf("%u\n", count);
    }
    return wide_index_range_success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;

    if (table != NULL && sql != NULL) {
        WideIndexRangeSelect select;
        if (wide_index_range_parse_select(table, sql, &select)) {
            return wide_index_range_execute(table, &select, output);
        }
    }
    return tinydb_generic_sql_try_execute_wide_index_base(table, sql, output);
}
