#include "engine.h"
#include "user_version.h"

#include <stdio.h>
#include <string.h>

static int fail(TinyDB* database, const char* message) {
    if (database != NULL) tinydb_close(database);
    fprintf(stderr, "USER_VERSION_API_PIN_FAIL: %s\n", message);
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
        fprintf(stderr, "USER_VERSION_API_PIN_FAIL: usage: tinydb_user_version_api_pin_probe DATABASE\n");
        return 1;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail(NULL, "unable to open database");

    TinyDBSqlResult result;
    if (tinydb_execute_sql(database,
                           "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                           &result) != TINYDB_SQL_SUCCESS ||
        tinydb_execute_sql(database,
                           "PRAGMA user_version = 77;",
                           &result) != TINYDB_SQL_SUCCESS) {
        return fail(database, "unable to seed catalog and user_version");
    }

    char sql[256];
    for (uint32_t row_id = 1u; row_id <= 260u; row_id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO archive VALUES (%u, 'archive-%u', 'a%u@example.com');",
                 row_id,
                 row_id,
                 row_id);
        if (tinydb_execute_sql(database, sql, &result) != TINYDB_SQL_SUCCESS) {
            return fail(database, "unable to grow archive beyond buffer pool");
        }
    }

    Table* table = tinydb_table(database);
    pager_checkpoint(table->pager);
    if (table->pager->num_pages <= MAX_BUFFER_POOL_SIZE) {
        return fail(database, "fixture did not exceed buffer pool");
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
            return fail(database, "unable to pin complete buffer-pool fixture");
        }
        owner_count++;
    }

    uint32_t version = UINT32_MAX;
    char message[160];
    if (db_try_get_user_version(table,
                                &version,
                                message,
                                sizeof(message))) {
        (void)release_handles(owners, owner_count);
        return fail(database, "try-get unexpectedly succeeded with every frame pinned");
    }
    if (version != 0u || strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        return fail(database, "try-get did not fail closed with BUSY diagnostic");
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        (void)release_handles(owners, owner_count);
        return fail(database, "unable to free one frame");
    }

    version = 0u;
    memset(message, 0, sizeof(message));
    if (!db_try_get_user_version(table,
                                 &version,
                                 message,
                                 sizeof(message)) ||
        version != 77u || message[0] != '\0') {
        (void)release_handles(owners, owner_count);
        return fail(database, "try-get did not recover with one free frame and value 77");
    }

    if (!release_handles(owners, owner_count)) {
        return fail(database, "unable to release pin-pressure owners");
    }

    if (!db_try_get_user_version(table, &version, NULL, 0u) || version != 77u) {
        return fail(database, "message-less try-get failed after pressure release");
    }

    tinydb_close(database);
    printf("USER_VERSION_API_PIN_OK busy_nonfatal=yes zero_on_failure=yes one_free_frame_success=yes value_preserved=yes optional_message=yes\n");
    return 0;
}
