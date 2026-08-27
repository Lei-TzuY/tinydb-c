#include "diagnostics.h"
#include "engine.h"

static bool execute_ok(TinyDB* database, const char* sql) {
    TinyDBSqlResult result;
    TinyDBSqlStatus status = tinydb_execute_sql(database, sql, &result);
    if (status != TINYDB_SQL_SUCCESS) {
        fprintf(stderr,
                "SQL failed: %s\nstatus=%s message=%s execute=%d route=%d\n",
                sql,
                tinydb_sql_status_string(status),
                result.message,
                (int)result.execute_result,
                (int)result.route_result);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: tinydb_engine_api_probe <database>\n");
        return 2;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) {
        fprintf(stderr, "unable to open database\n");
        return 1;
    }

    if (!execute_ok(database,
                    "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);")) {
        tinydb_close(database);
        return 1;
    }
    if (!execute_ok(database,
                    "INSERT INTO users VALUES (7, 'main-seven', 'main7@example.com');")) {
        tinydb_close(database);
        return 1;
    }

    char sql[256];
    for (uint32_t id = 1; id <= 20; id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO archive VALUES (%u, 'archive-%u', 'archive%u@example.com');",
                 id,
                 id,
                 id);
        if (!execute_ok(database, sql)) {
            tinydb_close(database);
            return 1;
        }
    }

    Table* table = tinydb_table(database);
    const TableSchema* users_schema = tinydb_find_table_schema(table, "users");
    const TableSchema* archive_schema = tinydb_find_table_schema(table, "archive");
    if (users_schema == NULL || archive_schema == NULL ||
        users_schema->root_page_num == archive_schema->root_page_num) {
        fprintf(stderr, "catalog roots are not independent\n");
        tinydb_close(database);
        return 1;
    }

    uint32_t archive_root = archive_schema->root_page_num;
    TinyDBTreeStats users_stats;
    TinyDBTreeStats archive_stats;
    if (!tinydb_get_tree_stats(table, "users", &users_stats) ||
        !tinydb_get_tree_stats(table, "archive", &archive_stats)) {
        fprintf(stderr, "unable to read tree stats\n");
        tinydb_close(database);
        return 1;
    }
    if (users_stats.total_rows != 1 ||
        archive_stats.total_rows != 20 ||
        archive_stats.leaf_pages < 2 ||
        archive_stats.internal_pages < 1 ||
        archive_stats.height < 2) {
        fprintf(stderr,
                "unexpected stats users=%u archive=%u leaf=%u internal=%u height=%u\n",
                users_stats.total_rows,
                archive_stats.total_rows,
                archive_stats.leaf_pages,
                archive_stats.internal_pages,
                archive_stats.height);
        tinydb_close(database);
        return 1;
    }

    char diagnostic[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_table_tree(table, "users", diagnostic, sizeof(diagnostic)) ||
        !tinydb_check_table_tree(table, "archive", diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "tree integrity failed: %s\n", diagnostic);
        tinydb_close(database);
        return 1;
    }

    TinyDBSqlResult join_result;
    if (tinydb_execute_sql(database,
                           "SELECT * FROM users JOIN archive ON users.id = archive.id;",
                           &join_result) != TINYDB_SQL_SUCCESS ||
        !join_result.join_handled) {
        fprintf(stderr, "cross-root JOIN did not use the engine JOIN path\n");
        tinydb_close(database);
        return 1;
    }

    tinydb_close(database);
    database = tinydb_open(argv[1]);
    if (database == NULL) {
        fprintf(stderr, "unable to reopen database\n");
        return 1;
    }

    table = tinydb_table(database);
    archive_schema = tinydb_find_table_schema(table, "archive");
    if (archive_schema == NULL || archive_schema->root_page_num != archive_root) {
        fprintf(stderr, "archive root did not persist across reopen\n");
        tinydb_close(database);
        return 1;
    }
    if (!tinydb_get_tree_stats(table, "archive", &archive_stats) ||
        archive_stats.total_rows != 20) {
        fprintf(stderr, "archive rows did not persist across reopen\n");
        tinydb_close(database);
        return 1;
    }
    if (!tinydb_get_tree_stats(table, "users", &users_stats) ||
        users_stats.total_rows != 1) {
        fprintf(stderr, "users rows changed across reopen\n");
        tinydb_close(database);
        return 1;
    }

    printf("ENGINE_API_OK users_root=%u archive_root=%u archive_rows=%u archive_height=%u\n",
           users_schema != NULL ? users_stats.root_page_num : 0,
           archive_stats.root_page_num,
           archive_stats.total_rows,
           archive_stats.height);
    tinydb_close(database);
    return 0;
}
