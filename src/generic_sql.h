#ifndef GENERIC_SQL_H
#define GENERIC_SQL_H

#include "record.h"
#include "vm.h"

#define TINYDB_GENERIC_SQL_MESSAGE_MAX 256

typedef enum {
    TINYDB_GENERIC_SQL_NOT_APPLICABLE = 0,
    TINYDB_GENERIC_SQL_SUCCESS,
    TINYDB_GENERIC_SQL_SYNTAX_ERROR,
    TINYDB_GENERIC_SQL_EXECUTE_ERROR
} TinyDBGenericSqlStatus;

typedef struct {
    TinyDBGenericSqlStatus status;
    StatementType statement_type;
    ExecuteResult execute_result;
    bool statement_type_valid;
    bool executed;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX];
} TinyDBGenericSqlResult;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(Table* table,
                                                       const char* sql,
                                                       TinyDBGenericSqlResult* result);

#endif /* GENERIC_SQL_H */
