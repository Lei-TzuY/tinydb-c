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
    STATEMENT_DROP_INDEX,
    STATEMENT_VACUUM,
    STATEMENT_SAVEPOINT,
    STATEMENT_ROLLBACK_TO,
    STATEMENT_RELEASE_SAVEPOINT,
    STATEMENT_CHECKPOINT,
    STATEMENT_PRAGMA_INTEGRITY_CHECK,
    STATEMENT_PRAGMA_USER_VERSION,
    STATEMENT_PRAGMA_SET_USER_VERSION,
    STATEMENT_PRAGMA_TABLE_INFO,
    STATEMENT_PRAGMA_INDEX_LIST,
    STATEMENT_CREATE_TABLE,
    STATEMENT_PREPARE,
    STATEMENT_EXECUTE_PREPARED
} StatementType;

typedef struct {
    char table_name[32];
    uint32_t num_columns;
    char col_names[16][32];
    char col_types[16][16];
    bool has_fk;
    char fk_col[32];
    char fk_parent_table[32];
    char fk_parent_col[32];
} CreateTableStatement;

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
    AGGREGATE_MAX,
    AGGREGATE_SUM,
    AGGREGATE_AVG
} AggregateType;

typedef struct {
    char          table_name[32];
    bool          has_id_filter;
    uint32_t      id;
    CompareOp     id_op;       /* operator for the WHERE id clause */
    bool          has_id_min_filter;
    uint32_t      id_min;
    CompareOp     id_min_op;   /* > or >= */
    bool          has_id_max_filter;
    uint32_t      id_max;
    CompareOp     id_max_op;   /* < or <= */
    bool          has_username_filter;
    char          username[COLUMN_USERNAME_SIZE + 1];
    bool          has_username_like;
    char          username_like[COLUMN_USERNAME_SIZE + 1];
    bool          has_email_filter;
    char          email[COLUMN_EMAIL_SIZE + 1];
    bool          has_email_like;
    char          email_like[COLUMN_EMAIL_SIZE + 1];
    AggregateType aggregate;
    bool          has_order_desc; /* ORDER BY id DESC */
    bool          has_limit;
    uint32_t      limit;
    bool          has_join;
    char          join_table[32];
    char          join_left_col[32];
    char          join_right_col[32];
    bool          has_group_by;
    char          group_by_col[32];
    bool          has_project_col;
    char          project_col[32];
    bool          has_having;
    AggregateType having_agg;
    CompareOp     having_op;
    uint32_t      having_val;
} SelectStatement;

typedef struct {
    bool set_username;
    char username[COLUMN_USERNAME_SIZE + 1];
    bool set_email;
    char email[COLUMN_EMAIL_SIZE + 1];
    uint32_t id;
} UpdateStatement;

typedef struct {
    char name[64];
} SavepointStatement;

typedef struct {
    uint32_t user_version;
} PragmaStatement;

typedef struct {
    char name[32];
    char sql_template[256];
} PrepareStatement;

typedef struct {
    char name[32];
    uint32_t param_val;
} ExecutePreparedStatement;

typedef struct {
    char name[64];
    char table_name[32];
    char column_name[32];
} CreateIndexStatement;

typedef struct {
    char name[64];
} DropIndexStatement;

typedef struct {
    StatementType type;
    bool explain;
    char table_name[32];
    Row row_to_insert;
    SelectStatement select;
    CreateTableStatement create_table;
    CreateIndexStatement create_index;
    DropIndexStatement drop_index;
    PrepareStatement prepare;
    ExecutePreparedStatement execute_prepared;
    uint32_t delete_id;
    bool delete_all;
    UpdateStatement update;
    SavepointStatement savepoint;
    PragmaStatement pragma;
} Statement;

PrepareResult prepare_statement(const char* input, Statement* statement);

#endif // COMPILER_H
