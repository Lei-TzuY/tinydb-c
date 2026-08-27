#include "engine.h"

#include <ctype.h>

static void initialize_result(TinyDBSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_SQL_SUCCESS;
    result->prepare_result = PREPARE_SUCCESS;
    result->execute_result = EXECUTE_SUCCESS;
    result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
}

static void set_result_message(TinyDBSqlResult* result, const char* message) {
    if (message == NULL) {
        result->message[0] = '\0';
        return;
    }
    snprintf(result->message, sizeof(result->message), "%s", message);
}

static bool consume_ci_word(const char** input, const char* word) {
    const char* p = *input;
    const char* w = word;

    while (isspace((unsigned char)*p)) p++;
    while (*w != '\0' &&
           tolower((unsigned char)*p) == tolower((unsigned char)*w)) {
        p++;
        w++;
    }
    if (*w != '\0') return false;
    if (isalnum((unsigned char)*p) || *p == '_') return false;

    *input = p;
    return true;
}

static bool extract_explain_analyze_query(const char* sql, const char** query) {
    const char* current = sql;
    if (!consume_ci_word(&current, "explain") ||
        !consume_ci_word(&current, "analyze")) {
        return false;
    }
    while (isspace((unsigned char)*current)) current++;
    *query = current;
    return true;
}

static TinyDBSqlStatus execute_explain_analyze(TinyDB* database,
                                                const char* query,
                                                TinyDBSqlResult* result) {
    Statement statement;
    MultiTableRouteScope route_scope;

    result->prepare_result = prepare_statement(query, &statement);
    if (result->prepare_result != PREPARE_SUCCESS ||
        statement.type != STATEMENT_SELECT) {
        result->status = TINYDB_SQL_SYNTAX_ERROR;
        set_result_message(result,
                           "EXPLAIN ANALYZE currently requires a SELECT statement");
        return result->status;
    }

    result->statement_type = statement.type;
    result->statement_type_valid = true;
    result->route_result = multitable_begin_statement_scope(
        database->table, &statement, query, &route_scope);
    if (result->route_result != MULTITABLE_ROUTE_OK &&
        result->route_result != MULTITABLE_ROUTE_NOT_APPLICABLE) {
        result->status = TINYDB_SQL_ROUTE_ERROR;
        set_result_message(result, multitable_route_error(result->route_result));
        return result->status;
    }

    result->has_profile = query_profile_execute(
        &statement, database->table, &result->profile);
    multitable_end_statement_scope(database->table, &route_scope);

    if (!result->has_profile) {
        result->status = TINYDB_SQL_EXECUTE_ERROR;
        set_result_message(result, "EXPLAIN ANALYZE could not profile this statement");
        return result->status;
    }

    result->execute_result = result->profile.execute_result;
    result->executed = result->profile.execute_result == EXECUTE_SUCCESS;
    if (result->profile.plan_result != EXECUTE_SUCCESS ||
        result->profile.execute_result != EXECUTE_SUCCESS) {
        result->status = TINYDB_SQL_EXECUTE_ERROR;
        set_result_message(result, "EXPLAIN ANALYZE execution failed");
        return result->status;
    }

    result->status = TINYDB_SQL_SUCCESS;
    return result->status;
}

TinyDB* tinydb_open(const char* filename) {
    if (filename == NULL || filename[0] == '\0' ||
        strlen(filename) >= TINYDB_ENGINE_FILENAME_MAX) {
        return NULL;
    }

    TinyDB* database = (TinyDB*)calloc(1, sizeof(*database));
    if (database == NULL) return NULL;

    snprintf(database->filename, sizeof(database->filename), "%s", filename);
    database->table = db_open(filename);
    if (database->table == NULL) {
        free(database);
        return NULL;
    }

    if (!multitable_catalog_load(database->table, database->filename)) {
        printf("Warning: schema catalog could not be loaded; using in-memory catalog state.\n");
    }
    return database;
}

void tinydb_close(TinyDB* database) {
    if (database == NULL) return;
    if (database->table != NULL) {
        db_close(database->table);
        database->table = NULL;
    }
    free(database);
}

Table* tinydb_table(TinyDB* database) {
    return database != NULL ? database->table : NULL;
}

const char* tinydb_filename(const TinyDB* database) {
    return database != NULL ? database->filename : NULL;
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    TinyDBSqlResult local_result;
    TinyDBSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (database == NULL || database->table == NULL || sql == NULL) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output, "invalid database handle or SQL string");
        return output->status;
    }

    const char* explain_query = NULL;
    if (extract_explain_analyze_query(sql, &explain_query)) {
        return execute_explain_analyze(database, explain_query, output);
    }

    Statement statement;
    output->prepare_result = prepare_statement(sql, &statement);
    if (output->prepare_result == PREPARE_SYNTAX_ERROR) {
        output->status = TINYDB_SQL_SYNTAX_ERROR;
        set_result_message(output, "syntax error");
        return output->status;
    }
    if (output->prepare_result == PREPARE_UNRECOGNIZED_STATEMENT) {
        output->status = TINYDB_SQL_UNRECOGNIZED_STATEMENT;
        set_result_message(output, "unrecognized statement");
        return output->status;
    }

    output->statement_type = statement.type;
    output->statement_type_valid = true;
    Table* table = database->table;

    if (multitable_is_schema_ddl(statement.type) && table->in_transaction) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output, "schema DDL is not allowed inside a transaction");
        return output->status;
    }

    if (table->catalog.num_tables > 1 && statement.type == STATEMENT_VACUUM) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "VACUUM/VACUUM INTO is disabled for multi-table databases until compaction preserves every table root and schema sidecar");
        return output->status;
    }

    if (table->catalog.num_tables > 1 &&
        statement.type == STATEMENT_EXECUTE_PREPARED) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "EXECUTE PREPARED is disabled for multi-table databases until bound SQL participates in root routing");
        return output->status;
    }

    if (table->catalog.num_tables > 1 &&
        statement.type == STATEMENT_ALTER_TABLE &&
        statement.alter_table.is_add_column) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "ALTER TABLE ADD COLUMN is disabled for multi-table fixed-Row storage until physical row migration is implemented");
        return output->status;
    }

    if (statement.type == STATEMENT_CREATE_INDEX &&
        !multitable_index_target_supported(table,
                                           statement.create_index.table_name)) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "secondary indexes on non-primary table roots are not routed safely yet");
        return output->status;
    }

    if (!statement.explain &&
        statement.type == STATEMENT_SELECT &&
        statement.select.has_join) {
        bool handled = false;
        ExecuteResult join_execute_result = EXECUTE_SUCCESS;
        output->route_result = multitable_execute_join(
            table, &statement, &handled, &join_execute_result);
        output->join_handled = handled;
        if (handled) {
            if (output->route_result != MULTITABLE_ROUTE_OK) {
                output->status = TINYDB_SQL_ROUTE_ERROR;
                set_result_message(output,
                                   multitable_route_error(output->route_result));
                return output->status;
            }
            output->execute_result = join_execute_result;
            output->executed = join_execute_result == EXECUTE_SUCCESS;
            if (join_execute_result != EXECUTE_SUCCESS) {
                output->status = TINYDB_SQL_EXECUTE_ERROR;
                set_result_message(output, "JOIN execution failed");
                return output->status;
            }
            output->status = TINYDB_SQL_SUCCESS;
            return output->status;
        }
    }

    MultiTableRouteScope route_scope;
    output->route_result = multitable_begin_statement_scope(
        table, &statement, sql, &route_scope);
    if (output->route_result != MULTITABLE_ROUTE_OK &&
        output->route_result != MULTITABLE_ROUTE_NOT_APPLICABLE) {
        output->status = TINYDB_SQL_ROUTE_ERROR;
        set_result_message(output, multitable_route_error(output->route_result));
        return output->status;
    }

    if (statement.type == STATEMENT_DELETE &&
        statement.delete_all &&
        route_scope.active &&
        table->catalog.num_tables > 1) {
        output->execute_result = multitable_execute_delete_all(
            &statement, table, &route_scope);
    } else {
        output->execute_result = execute_statement(&statement, table);
    }
    multitable_end_statement_scope(table, &route_scope);
    output->executed = true;

    if (output->execute_result != EXECUTE_SUCCESS) {
        output->status = TINYDB_SQL_EXECUTE_ERROR;
        return output->status;
    }

    if (multitable_is_schema_ddl(statement.type)) {
        db_checkpoint(table);
        if (!multitable_catalog_save(table, database->filename)) {
            output->status = TINYDB_SQL_CATALOG_PERSIST_ERROR;
            set_result_message(output, "schema catalog could not be persisted");
            return output->status;
        }
        output->schema_persisted = true;
    }

    output->status = TINYDB_SQL_SUCCESS;
    return output->status;
}

const char* tinydb_sql_status_string(TinyDBSqlStatus status) {
    switch (status) {
        case TINYDB_SQL_SUCCESS:
            return "success";
        case TINYDB_SQL_SYNTAX_ERROR:
            return "syntax error";
        case TINYDB_SQL_UNRECOGNIZED_STATEMENT:
            return "unrecognized statement";
        case TINYDB_SQL_POLICY_ERROR:
            return "policy error";
        case TINYDB_SQL_ROUTE_ERROR:
            return "route error";
        case TINYDB_SQL_EXECUTE_ERROR:
            return "execution error";
        case TINYDB_SQL_CATALOG_PERSIST_ERROR:
            return "catalog persistence error";
    }
    return "unknown";
}
