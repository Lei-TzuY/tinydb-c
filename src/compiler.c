#include "compiler.h"

#include <ctype.h>
#include <errno.h>

typedef struct {
    const char* current;
} Parser;

static void skip_spaces(Parser* parser) {
    while (isspace((unsigned char)*parser->current)) {
        parser->current++;
    }
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_keyword(Parser* parser, const char* keyword) {
    const char* input;
    const char* expected;

    skip_spaces(parser);
    input = parser->current;
    expected = keyword;

    while (*expected != '\0' &&
           tolower((unsigned char)*input) == tolower((unsigned char)*expected)) {
        input++;
        expected++;
    }

    if (*expected != '\0' || is_identifier_char(*input)) {
        return false;
    }

    parser->current = input;
    return true;
}

static bool consume_character(Parser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) {
        return false;
    }

    parser->current++;
    return true;
}

static bool consume_statement_end(Parser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') {
        parser->current++;
        skip_spaces(parser);
    }
    return *parser->current == '\0';
}

static bool parse_identifier(Parser* parser) {
    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }

    while (is_identifier_char(*parser->current)) {
        parser->current++;
    }
    return true;
}

static bool parse_identifier_into(Parser* parser,
                                  char* destination,
                                  size_t capacity) {
    const char* start;
    size_t length;

    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }

    start = parser->current;
    while (is_identifier_char(*parser->current)) {
        parser->current++;
    }

    length = (size_t)(parser->current - start);
    if (length == 0 || length >= capacity) {
        return false;
    }

    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static bool parse_uint32(Parser* parser, uint32_t* value) {
    char* end;
    unsigned long long parsed;

    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) {
        return false;
    }

    errno = 0;
    parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX) {
        return false;
    }

    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool consume_compare_op(Parser* parser, CompareOp* op) {
    skip_spaces(parser);
    /* Check two-character operators first so >= isn't parsed as > */
    if (parser->current[0] == '>' && parser->current[1] == '=') {
        *op = COMPARE_GTE; parser->current += 2; return true;
    }
    if (parser->current[0] == '<' && parser->current[1] == '=') {
        *op = COMPARE_LTE; parser->current += 2; return true;
    }
    if (parser->current[0] == '>') { *op = COMPARE_GT; parser->current += 1; return true; }
    if (parser->current[0] == '<') { *op = COMPARE_LT; parser->current += 1; return true; }
    if (parser->current[0] == '=') { *op = COMPARE_EQ; parser->current += 1; return true; }
    return false;
}

static bool parse_token(Parser* parser, char* destination, size_t capacity) {
    const char* start;
    size_t length;

    skip_spaces(parser);
    start = parser->current;
    while (*parser->current != '\0' &&
           !isspace((unsigned char)*parser->current) &&
           *parser->current != ';') {
        parser->current++;
    }

    length = (size_t)(parser->current - start);
    if (length == 0 || length >= capacity) {
        return false;
    }

    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static bool parse_sql_string(Parser* parser, char* destination, size_t capacity) {
    size_t length = 0;

    skip_spaces(parser);
    if (*parser->current != '\'') {
        return false;
    }
    parser->current++;

    while (*parser->current != '\0') {
        char value = *parser->current++;
        if (value == '\'') {
            if (*parser->current == '\'') {
                parser->current++;
                value = '\'';
            } else {
                destination[length] = '\0';
                return true;
            }
        }

        if (length + 1 >= capacity) {
            return false;
        }
        destination[length++] = value;
    }

    return false;
}

static bool parse_legacy_insert(Parser* parser, Statement* statement) {
    return parse_uint32(parser, &statement->row_to_insert.id) &&
           parse_token(parser, statement->row_to_insert.username,
                       sizeof(statement->row_to_insert.username)) &&
           parse_token(parser, statement->row_to_insert.email,
                       sizeof(statement->row_to_insert.email)) &&
           consume_statement_end(parser);
}

static bool parse_sql_insert(Parser* parser, Statement* statement) {
    return parse_identifier(parser) &&
           consume_keyword(parser, "values") &&
           consume_character(parser, '(') &&
           parse_uint32(parser, &statement->row_to_insert.id) &&
           consume_character(parser, ',') &&
           parse_sql_string(parser, statement->row_to_insert.username,
                            sizeof(statement->row_to_insert.username)) &&
           consume_character(parser, ',') &&
           parse_sql_string(parser, statement->row_to_insert.email,
                            sizeof(statement->row_to_insert.email)) &&
           consume_character(parser, ')') &&
           consume_statement_end(parser);
}

static bool parse_insert(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_INSERT;
    if (consume_keyword(parser, "into")) {
        return parse_sql_insert(parser, statement);
    }
    return parse_legacy_insert(parser, statement);
}

static bool parse_delete(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_DELETE;
    if (!consume_keyword(parser, "from") || !parse_identifier(parser)) {
        return false;
    }
    if (consume_statement_end(parser)) {
        statement->delete_all = true;
        return true;
    }
    return consume_keyword(parser, "where") &&
           consume_keyword(parser, "id") &&
           consume_character(parser, '=') &&
           parse_uint32(parser, &statement->delete_id) &&
           consume_statement_end(parser);
}

static bool parse_update_set_clause(Parser* parser, Statement* statement) {
    bool parsed_any = false;
    while (true) {
        skip_spaces(parser);
        if (consume_keyword(parser, "username")) {
            if (!consume_character(parser, '=')) return false;
            if (!parse_sql_string(parser, statement->update.username,
                                  sizeof(statement->update.username))) return false;
            statement->update.set_username = true;
        } else if (consume_keyword(parser, "email")) {
            if (!consume_character(parser, '=')) return false;
            if (!parse_sql_string(parser, statement->update.email,
                                  sizeof(statement->update.email))) return false;
            statement->update.set_email = true;
        } else {
            break;
        }
        parsed_any = true;
        skip_spaces(parser);
        if (*parser->current != ',') break;
        parser->current++;
    }
    return parsed_any;
}

static bool parse_update(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_UPDATE;
    return parse_identifier(parser) &&
           consume_keyword(parser, "set") &&
           parse_update_set_clause(parser, statement) &&
           consume_keyword(parser, "where") &&
           consume_keyword(parser, "id") &&
           consume_character(parser, '=') &&
           parse_uint32(parser, &statement->update.id) &&
           consume_statement_end(parser);
}

static bool parse_select(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_SELECT;

    /* Bare "select" with nothing following */
    if (consume_statement_end(parser)) return true;

    /* Projection: COUNT(*), MIN(id), MAX(id), or * */
    if (consume_keyword(parser, "count")) {
        if (!consume_character(parser, '(') ||
            !consume_character(parser, '*') ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_COUNT;
    } else if (consume_keyword(parser, "min")) {
        if (!consume_character(parser, '(') ||
            !consume_keyword(parser, "id") ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_MIN;
    } else if (consume_keyword(parser, "max")) {
        if (!consume_character(parser, '(') ||
            !consume_keyword(parser, "id") ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_MAX;
    } else {
        if (!consume_character(parser, '*')) return false;
    }

    if (!consume_keyword(parser, "from") || !parse_identifier(parser))
        return false;

    if (consume_statement_end(parser)) return true;

    /* Optional WHERE id <op> N or username = 'value' */
    if (consume_keyword(parser, "where")) {
        if (consume_keyword(parser, "id")) {
            CompareOp op;
            if (!consume_compare_op(parser, &op)) return false;
            if (!parse_uint32(parser, &statement->select.id)) return false;
            statement->select.has_id_filter = true;
            statement->select.id_op = op;
        } else if (consume_keyword(parser, "username")) {
            if (!consume_character(parser, '=')) return false;
            if (!parse_sql_string(parser, statement->select.username,
                                  sizeof(statement->select.username))) return false;
            statement->select.has_username_filter = true;
        } else {
            return false;
        }
    }

    /* Optional ORDER BY id DESC */
    if (consume_keyword(parser, "order")) {
        if (!consume_keyword(parser, "by") ||
            !consume_keyword(parser, "id") ||
            !consume_keyword(parser, "desc"))
            return false;
        statement->select.has_order_desc = true;
    }

    /* Optional LIMIT N */
    if (consume_keyword(parser, "limit")) {
        if (!parse_uint32(parser, &statement->select.limit)) return false;
        statement->select.has_limit = true;
    }

    return consume_statement_end(parser);
}

static bool parse_create_index(Parser* parser, Statement* statement) {
    char index_name[64];
    char table_name[64];

    statement->type = STATEMENT_CREATE_INDEX;
    if (!consume_keyword(parser, "index") ||
        !parse_identifier_into(parser, index_name, sizeof(index_name)) ||
        !consume_keyword(parser, "on") ||
        !parse_identifier_into(parser, table_name, sizeof(table_name)) ||
        strcmp(index_name, "idx_users_username") != 0 ||
        strcmp(table_name, "users") != 0) {
        return false;
    }
    return consume_character(parser, '(') &&
           consume_keyword(parser, "username") &&
           consume_character(parser, ')') &&
           consume_statement_end(parser);
}

static bool parse_drop_index(Parser* parser, Statement* statement) {
    char index_name[64];

    statement->type = STATEMENT_DROP_INDEX;
    return consume_keyword(parser, "index") &&
           parse_identifier_into(parser, index_name, sizeof(index_name)) &&
           strcmp(index_name, "idx_users_username") == 0 &&
           consume_statement_end(parser);
}

PrepareResult prepare_statement(const char* input, Statement* statement) {
    Parser parser;

    memset(statement, 0, sizeof(*statement));
    parser.current = input;

    if (consume_keyword(&parser, "explain")) {
        statement->explain = true;
        if (!consume_keyword(&parser, "select") ||
            !parse_select(&parser, statement)) {
            return PREPARE_SYNTAX_ERROR;
        }
        return PREPARE_SUCCESS;
    }

    if (consume_keyword(&parser, "insert")) {
        return parse_insert(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "select")) {
        return parse_select(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "delete")) {
        return parse_delete(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "begin")) {
        statement->type = STATEMENT_BEGIN;
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "commit")) {
        statement->type = STATEMENT_COMMIT;
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "rollback")) {
        statement->type = STATEMENT_ROLLBACK;
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "update")) {
        return parse_update(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "create")) {
        return parse_create_index(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "drop")) {
        return parse_drop_index(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}
