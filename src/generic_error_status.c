#include "generic_sql.h"

#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_diagnostic_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

/*
 * Generic SQL carries a rich status/message pair, while ExecuteResult is a
 * legacy VM enum that has no "validation error" member. The historical
 * fallback reused EXECUTE_KEY_NOT_FOUND for INSERT validation failures, which
 * caused callers such as the REPL to discard the precise record-layer message
 * and print a misleading missing-key error.
 *
 * Preserve duplicate-key as its real constraint code, but for any other
 * generic INSERT execution failure that already has a diagnostic message,
 * leave the SQL status as EXECUTE_ERROR and neutralize the unrelated legacy
 * sub-code. `executed` remains false, so embedded callers still have an
 * unambiguous failure signal while retaining the actionable message.
 */
TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_try_execute_diagnostic_base(table, sql, result);

    if (result != NULL &&
        status == TINYDB_GENERIC_SQL_EXECUTE_ERROR &&
        result->statement_type_valid &&
        result->statement_type == STATEMENT_INSERT &&
        result->execute_result == EXECUTE_KEY_NOT_FOUND &&
        result->message[0] != '\0') {
        result->execute_result = EXECUTE_SUCCESS;
    }

    return status;
}
