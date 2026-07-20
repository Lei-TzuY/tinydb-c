#ifndef COMPILER_H
#define COMPILER_H

#include "common.h"
#include "table.h"

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_DELETE,
    STATEMENT_BEGIN,
    STATEMENT_COMMIT,
    STATEMENT_ROLLBACK,
    STATEMENT_UPDATE,
    STATEMENT_CREATE_INDEX,
    STATEMENT_DROP_INDEX
} StatementType;

/* COMPARE_EQ must be 0 so zero-initialised statements default to = */
typedef enum {
    COMPARE_EQ = 0,
    COMPARE_GT,
    COMPARE_GTE,
    COMPARE_LT,
    COMPARE_LTE
} CompareOp;

/* AGGREGATE_NONE must be 0 so zero-initialised statements default to no aggregate */
typedef enum {
    AGGREGATE_NONE = 0,
    AGGREGATE_COUNT,
    AGGREGATE_MIN,
    AGGREGATE_MAX
} AggregateType;

typedef struct {
    bool          has_id_filter;
    uint32_t      id;
    CompareOp     id_op;       /* operator for the WHERE id clause */
    bool          has_username_filter;
    char          username[COLUMN_USERNAME_SIZE + 1];
    AggregateType aggregate;
    bool          has_order_desc; /* ORDER BY id DESC */
    bool          has_limit;
    uint32_t      limit;
} SelectStatement;

typedef struct {
    bool set_username;
    char username[COLUMN_USERNAME_SIZE + 1];
    bool set_email;
    char email[COLUMN_EMAIL_SIZE + 1];
    uint32_t id;
} UpdateStatement;

typedef struct {
    StatementType type;
    bool explain;
    Row row_to_insert;
    SelectStatement select;
    uint32_t delete_id;
    bool delete_all;
    UpdateStatement update;
} Statement;

PrepareResult prepare_statement(const char* input, Statement* statement);

#endif // COMPILER_H
