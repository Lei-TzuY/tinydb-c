#include "diagnostics.h"
#include "engine.h"

#include <stdio.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_integrity_delegate_base(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result);

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

    Table* table = database->table;
    bool ok = true;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* schema = &table->catalog.schemas[i];

        for (uint32_t j = 0; j < i; j++) {
            if (table->catalog.schemas[j].root_page_num == schema->root_page_num) {
                printf("Error: Tables '%s' and '%s' share root page %u.\n",
                       table->catalog.schemas[j].name,
                       schema->name,
                       schema->root_page_num);
                ok = false;
            }
        }

        if (!tinydb_check_table_tree(table,
                                     schema->name,
                                     message,
                                     sizeof(message))) {
            printf("Error: Table '%s': %s\n", schema->name, message);
            ok = false;
        }
    }

    if (ok) {
        printf("ok\n");
        return output->status;
    }

    output->status = TINYDB_SQL_EXECUTE_ERROR;
    snprintf(output->message,
             sizeof(output->message),
             "%s",
             "database integrity check failed");
    return output->status;
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    if (database == NULL || database->table == NULL || sql == NULL) {
        return tinydb_execute_sql_integrity_delegate_base(database, sql, result);
    }

    Statement statement;
    memset(&statement, 0, sizeof(statement));
    if (prepare_statement(sql, &statement) == PREPARE_SUCCESS &&
        statement.type == STATEMENT_PRAGMA_INTEGRITY_CHECK) {
        return execute_integrity_check(database, result);
    }

    return tinydb_execute_sql_integrity_delegate_base(database, sql, result);
}
