#include "analyze_sql.h"
#include "diagnostics.h"
#include "engine.h"

#include <stdio.h>
#include <string.h>

#define TINYDB_PREPARED_ROUTE_CAPACITY 16u
#define TINYDB_PREPARED_ROUTE_SQL_MAX 256u
#define TINYDB_PREPARED_BOUND_SQL_MAX 512u
#define TINYDB_PREPARED_MAX_DEPTH 8u

typedef struct {
    char name[32];
    char sql_template[TINYDB_PREPARED_ROUTE_SQL_MAX];
} TinyDBPreparedRouteEntry;

static TinyDBPreparedRouteEntry prepared_routes[TINYDB_PREPARED_ROUTE_CAPACITY];
static uint32_t prepared_route_count = 0;

TinyDBSqlStatus tinydb_execute_sql_prepared_delegate_base(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result);

static void initialize_error_result(TinyDBSqlResult* result,
                                    TinyDBSqlStatus status,
                                    const char* message) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = status;
    result->prepare_result = PREPARE_SUCCESS;
    result->execute_result = EXECUTE_SUCCESS;
    result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    result->statement_type = STATEMENT_EXECUTE_PREPARED;
    result->statement_type_valid = true;
    result->executed = false;
    snprintf(result->message, sizeof(result->message), "%s", message);
}

static TinyDBSqlStatus map_analyze_result(
    TinyDBAnalyzeStatus analyze_status,
    const TinyDBAnalyzeResult* analyze_result,
    TinyDBSqlResult* result) {
    TinyDBSqlResult local_result;
    TinyDBSqlResult* output = result != NULL ? result : &local_result;
    memset(output, 0, sizeof(*output));
    output->prepare_result = PREPARE_SUCCESS;
    output->execute_result = EXECUTE_SUCCESS;
    output->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    output->statement_type_valid = false;
    output->executed = analyze_status == TINYDB_ANALYZE_SUCCESS;
    snprintf(output->message,
             sizeof(output->message),
             "%s",
             analyze_result->message);

    switch (analyze_status) {
        case TINYDB_ANALYZE_SUCCESS:
            output->status = TINYDB_SQL_SUCCESS;
            break;
        case TINYDB_ANALYZE_SYNTAX_ERROR:
            output->status = TINYDB_SQL_SYNTAX_ERROR;
            output->prepare_result = PREPARE_SYNTAX_ERROR;
            break;
        case TINYDB_ANALYZE_POLICY_ERROR:
            output->status = TINYDB_SQL_POLICY_ERROR;
            break;
        case TINYDB_ANALYZE_EXECUTE_ERROR:
            output->status = TINYDB_SQL_EXECUTE_ERROR;
            break;
        case TINYDB_ANALYZE_NOT_APPLICABLE:
        default:
            output->status = TINYDB_SQL_UNRECOGNIZED_STATEMENT;
            break;
    }
    return output->status;
}

static TinyDBSqlStatus execute_integrity_check(TinyDB* database,
                                               TinyDBSqlResult* result) {
    TinyDBSqlResult local_result;
    TinyDBSqlResult* output = result != NULL ? result : &local_result;
    memset(output, 0, sizeof(*output));
    output->status = TINYDB_SQL_SUCCESS;
    output->prepare_result = PREPARE_SUCCESS;
    output->execute_result = EXECUTE_SUCCESS;
    output->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    output->statement_type = STATEMENT_PRAGMA_INTEGRITY_CHECK;
    output->statement_type_valid = true;
    output->executed = true;

    TinyDBPageOwnershipStats ownership_stats;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (tinydb_check_database(database->table,
                              &ownership_stats,
                              message,
                              sizeof(message))) {
        printf("ok\n");
        return output->status;
    }

    printf("Error: %s\n", message);
    output->status = TINYDB_SQL_EXECUTE_ERROR;
    snprintf(output->message,
             sizeof(output->message),
             "database integrity check failed: %s",
             message);
    return output->status;
}

static TinyDBPreparedRouteEntry* find_prepared_route(const char* name) {
    if (name == NULL || name[0] == '\0') return NULL;
    for (uint32_t i = 0; i < prepared_route_count; i++) {
        if (strcmp(prepared_routes[i].name, name) == 0) {
            return &prepared_routes[i];
        }
    }
    return NULL;
}

static bool remember_prepared_route(const PrepareStatement* prepare) {
    if (prepare == NULL || prepare->name[0] == '\0' ||
        prepare->sql_template[0] == '\0') {
        return false;
    }

    TinyDBPreparedRouteEntry* existing = find_prepared_route(prepare->name);
    if (existing != NULL) {
        snprintf(existing->sql_template,
                 sizeof(existing->sql_template),
                 "%s",
                 prepare->sql_template);
        return true;
    }

    if (prepared_route_count >= TINYDB_PREPARED_ROUTE_CAPACITY) return false;
    TinyDBPreparedRouteEntry* entry = &prepared_routes[prepared_route_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", prepare->name);
    snprintf(entry->sql_template,
             sizeof(entry->sql_template),
             "%s",
             prepare->sql_template);
    return true;
}

static bool bind_prepared_route(const TinyDBPreparedRouteEntry* entry,
                                uint32_t parameter,
                                char* bound_sql,
                                size_t bound_sql_size) {
    if (entry == NULL || bound_sql == NULL || bound_sql_size == 0) return false;

    const char* placeholder = strchr(entry->sql_template, '?');
    int written = 0;
    if (placeholder == NULL) {
        written = snprintf(bound_sql,
                           bound_sql_size,
                           "%s",
                           entry->sql_template);
    } else {
        size_t prefix_length = (size_t)(placeholder - entry->sql_template);
        written = snprintf(bound_sql,
                           bound_sql_size,
                           "%.*s%u%s",
                           (int)prefix_length,
                           entry->sql_template,
                           parameter,
                           placeholder + 1);
    }
    return written >= 0 && (size_t)written < bound_sql_size;
}

static TinyDBSqlStatus execute_with_prepared_routing(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result,
    uint32_t depth) {
    if (database == NULL || database->table == NULL || sql == NULL) {
        return tinydb_execute_sql_prepared_delegate_base(database, sql, result);
    }

    TinyDBAnalyzeResult analyze_result;
    TinyDBAnalyzeStatus analyze_status = tinydb_analyze_try_execute(
        database->table, sql, &analyze_result);
    if (analyze_status != TINYDB_ANALYZE_NOT_APPLICABLE) {
        return map_analyze_result(analyze_status, &analyze_result, result);
    }

    Statement statement;
    memset(&statement, 0, sizeof(statement));
    PrepareResult prepare_result = prepare_statement(sql, &statement);
    if (prepare_result != PREPARE_SUCCESS) {
        return tinydb_execute_sql_prepared_delegate_base(database, sql, result);
    }

    if (statement.type == STATEMENT_PRAGMA_INTEGRITY_CHECK) {
        return execute_integrity_check(database, result);
    }

    if (statement.type == STATEMENT_PREPARE) {
        if (!remember_prepared_route(&statement.prepare)) {
            initialize_error_result(result,
                                    TINYDB_SQL_EXECUTE_ERROR,
                                    "prepared statement route registry is full or invalid");
            return TINYDB_SQL_EXECUTE_ERROR;
        }
        return tinydb_execute_sql_prepared_delegate_base(database, sql, result);
    }

    if (statement.type != STATEMENT_EXECUTE_PREPARED) {
        return tinydb_execute_sql_prepared_delegate_base(database, sql, result);
    }

    TinyDBPreparedRouteEntry* entry = find_prepared_route(
        statement.execute_prepared.name);
    if (entry == NULL) {
        char message[TINYDB_ENGINE_MESSAGE_MAX];
        snprintf(message,
                 sizeof(message),
                 "prepared statement '%s' not found",
                 statement.execute_prepared.name);
        initialize_error_result(result, TINYDB_SQL_EXECUTE_ERROR, message);
        return TINYDB_SQL_EXECUTE_ERROR;
    }

    if (depth >= TINYDB_PREPARED_MAX_DEPTH) {
        initialize_error_result(result,
                                TINYDB_SQL_POLICY_ERROR,
                                "prepared statement nesting exceeds the safe execution limit");
        return TINYDB_SQL_POLICY_ERROR;
    }

    char bound_sql[TINYDB_PREPARED_BOUND_SQL_MAX];
    if (!bind_prepared_route(entry,
                             statement.execute_prepared.param_val,
                             bound_sql,
                             sizeof(bound_sql))) {
        initialize_error_result(result,
                                TINYDB_SQL_EXECUTE_ERROR,
                                "bound prepared SQL exceeds execution buffer capacity");
        return TINYDB_SQL_EXECUTE_ERROR;
    }

    return execute_with_prepared_routing(database,
                                         bound_sql,
                                         result,
                                         depth + 1u);
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    return execute_with_prepared_routing(database, sql, result, 0u);
}
