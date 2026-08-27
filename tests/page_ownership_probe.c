#include "diagnostics.h"
#include "engine.h"

#include <stdio.h>
#include <string.h>

static int find_schema_index(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int fail(const char* message) {
    fprintf(stderr, "PAGE_OWNERSHIP_FAIL: %s\n", message);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: tinydb_page_ownership_probe DATABASE");

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail("unable to open database");

    TinyDBSqlResult result;
    if (tinydb_execute_sql(database,
                           "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                           &result) != TINYDB_SQL_SUCCESS ||
        tinydb_execute_sql(database,
                           "INSERT INTO users VALUES (1, 'main', 'main@example.com');",
                           &result) != TINYDB_SQL_SUCCESS ||
        tinydb_execute_sql(database,
                           "INSERT INTO archive VALUES (1, 'copy', 'copy@example.com');",
                           &result) != TINYDB_SQL_SUCCESS) {
        tinydb_close(database);
        return fail("unable to seed multi-root database");
    }

    Table* table = tinydb_table(database);
    TinyDBPageOwnershipStats stats;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        stats.orphan_pages != 0 || stats.shared_pages != 0) {
        tinydb_close(database);
        return fail("clean database did not pass ownership check");
    }
    if (!tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("clean database did not pass whole-database diagnostics");
    }

    uint32_t orphan_page = get_unused_page_num(table->pager);
    (void)get_page(table->pager, orphan_page);
    if (tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        stats.orphan_pages != 1 || strstr(message, "unreachable") == NULL) {
        tinydb_close(database);
        return fail("orphan page was not detected");
    }
    if (tinydb_check_database(table, &stats, message, sizeof(message)) ||
        stats.orphan_pages != 1 || strstr(message, "page ownership:") == NULL) {
        tinydb_close(database);
        return fail("whole-database diagnostics accepted orphan page");
    }
    if (tinydb_execute_sql(database, "PRAGMA integrity_check;", &result) == TINYDB_SQL_SUCCESS) {
        tinydb_close(database);
        return fail("PRAGMA integrity_check accepted orphan page");
    }

    pager_free_page(table->pager, orphan_page);
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        !tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("freeing orphan page did not restore ownership validity");
    }

    int users_index = find_schema_index(table, "users");
    int archive_index = find_schema_index(table, "archive");
    if (users_index < 0 || archive_index < 0) {
        tinydb_close(database);
        return fail("catalog schemas missing");
    }

    uint32_t archive_root = table->catalog.schemas[archive_index].root_page_num;
    table->catalog.schemas[archive_index].root_page_num =
        table->catalog.schemas[users_index].root_page_num;

    if (tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        stats.shared_pages == 0 || strstr(message, "referenced by both") == NULL) {
        tinydb_close(database);
        return fail("shared root/page was not detected");
    }
    if (tinydb_check_database(table, &stats, message, sizeof(message)) ||
        strstr(message, "share root page") == NULL) {
        tinydb_close(database);
        return fail("whole-database diagnostics accepted duplicate table roots");
    }
    if (tinydb_execute_sql(database, "PRAGMA integrity_check;", &result) == TINYDB_SQL_SUCCESS) {
        tinydb_close(database);
        return fail("PRAGMA integrity_check accepted shared root/page");
    }

    table->catalog.schemas[archive_index].root_page_num = archive_root;
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        !tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("restoring root did not restore ownership validity");
    }

    tinydb_close(database);
    printf("PAGE_OWNERSHIP_OK\n");
    return 0;
}
