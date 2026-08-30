#include "engine.h"

#include <stdio.h>
#include <string.h>

static int fail(TinyDB* database, const char* message) {
    if (database != NULL) tinydb_close(database);
    fprintf(stderr, "INTEGRITY_API_PIN_FAIL: %s\n", message);
    return 1;
}

static bool release_handles(PagerPageHandle* handles, uint32_t count) {
    bool ok = true;
    for (uint32_t i = 0u; i < count; i++) {
        if (handles[i].pinned && !pager_release_page_handle(&handles[i])) {
            ok = false;
        }
    }
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "INTEGRITY_API_PIN_FAIL: usage: tinydb_integrity_api_pin_probe DATABASE\n");
        return 1;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail(NULL, "unable to open database");

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
        return fail(database, "unable to seed multi-root database");
    }

    char sql[256];
    for (uint32_t row_id = 2u; row_id <= 260u; row_id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO archive VALUES (%u, 'archive-%u', 'a%u@example.com');",
                 row_id,
                 row_id,
                 row_id);
        if (tinydb_execute_sql(database, sql, &result) != TINYDB_SQL_SUCCESS) {
            return fail(database, "unable to grow archive beyond the buffer pool");
        }
    }

    Table* table = tinydb_table(database);
    pager_checkpoint(table->pager);
    if (table->pager->num_pages <= MAX_BUFFER_POOL_SIZE) {
        return fail(database, "fixture did not exceed the buffer pool");
    }

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    memset(owners, 0, sizeof(owners));
    uint32_t owner_count = 0u;
    for (uint32_t page_num = 1u;
         page_num <= MAX_BUFFER_POOL_SIZE;
         page_num++) {
        if (!pager_pin_page_handle(table->pager,
                                   page_num,
                                   &owners[owner_count])) {
            (void)release_handles(owners, owner_count);
            return fail(database, "unable to pin complete buffer pool fixture");
        }
        owner_count++;
    }

    if (db_integrity_check(table)) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_integrity_check unexpectedly succeeded with every frame pinned");
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        (void)release_handles(owners, owner_count);
        return fail(database, "unable to free one frame");
    }
    if (!db_integrity_check(table)) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_integrity_check requires more than one free frame");
    }

    if (!release_handles(owners, owner_count)) {
        return fail(database, "unable to release pin-pressure owners");
    }

    uint32_t orphan_page = get_unused_page_num(table->pager);
    (void)get_page(table->pager, orphan_page);
    if (db_integrity_check(table)) {
        return fail(database, "db_integrity_check accepted an allocated orphan page");
    }

    pager_free_page(table->pager, orphan_page);
    if (!db_integrity_check(table)) {
        return fail(database, "db_integrity_check did not recover after freeing orphan page");
    }

    tinydb_close(database);
    printf("INTEGRITY_API_PIN_OK busy_nonfatal=yes one_free_frame_success=yes ownership_fail_closed=yes recovery=yes\n");
    return 0;
}
