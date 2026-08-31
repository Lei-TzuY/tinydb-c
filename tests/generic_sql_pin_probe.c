#include "engine.h"
#include "pager_try_pin.h"

#include <stdio.h>
#include <string.h>

static int fail(TinyDB* database,
                PagerPageHandle* owners,
                uint32_t owner_count,
                const char* message) {
    if (owners != NULL) {
        for (uint32_t i = 0u; i < owner_count; i++) {
            if (owners[i].pinned) (void)pager_release_page_handle(&owners[i]);
        }
    }
    if (database != NULL) tinydb_close(database);
    fprintf(stderr, "GENERIC_SQL_PIN_FAIL: %s\n", message);
    return 1;
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool release_handles(PagerPageHandle* owners, uint32_t owner_count) {
    bool ok = true;
    for (uint32_t i = 0u; i < owner_count; i++) {
        if (owners[i].pinned && !pager_release_page_handle(&owners[i])) {
            ok = false;
        }
    }
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "GENERIC_SQL_PIN_FAIL: usage: generic_sql_pin_probe DATABASE\n");
        return 1;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail(NULL, NULL, 0u, "unable to open database");

    TinyDBSqlResult result;
    if (tinydb_execute_sql(
            database,
            "CREATE TABLE compact (id INT, left_text VARCHAR(200), right_text VARCHAR(80));",
            &result) != TINYDB_SQL_SUCCESS) {
        return fail(database, NULL, 0u, "unable to create compact generic table");
    }

    char sql[256];
    for (uint32_t row_id = 1u; row_id <= 280u; row_id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO compact VALUES (%u, 'left-%u', 'right-%u');",
                 row_id,
                 row_id,
                 row_id);
        if (tinydb_execute_sql(database, sql, &result) != TINYDB_SQL_SUCCESS) {
            return fail(database, NULL, 0u, "unable to grow compact table beyond buffer pool");
        }
    }

    Table* table = tinydb_table(database);
    TableSchema* schema = find_schema(table, "compact");
    if (schema == NULL || schema->row_size != 286u) {
        return fail(database, NULL, 0u, "compact fixture schema was not the expected 286-byte generic row");
    }

    pager_checkpoint(table->pager);
    if (table->pager->num_pages <= MAX_BUFFER_POOL_SIZE + 1u) {
        return fail(database, NULL, 0u, "fixture did not allocate enough non-root pages");
    }

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    memset(owners, 0, sizeof(owners));
    uint32_t owner_count = 0u;
    for (uint32_t page_num = 0u;
         page_num < table->pager->num_pages && owner_count < MAX_BUFFER_POOL_SIZE;
         page_num++) {
        if (page_num == schema->root_page_num) continue;
        if (!pager_pin_page_handle(table->pager,
                                   page_num,
                                   &owners[owner_count])) {
            return fail(database,
                        owners,
                        owner_count,
                        "unable to pin complete non-root buffer-pool fixture");
        }
        owner_count++;
    }
    if (owner_count != MAX_BUFFER_POOL_SIZE) {
        return fail(database,
                    owners,
                    owner_count,
                    "fixture did not provide sixteen distinct non-root owners");
    }

    PagerPageHandle root_probe;
    PagerTryPinStatus root_status = pager_try_pin_existing_page_handle(
        table->pager, schema->root_page_num, &root_probe);
    if (root_status != PAGER_TRY_PIN_BUSY) {
        if (root_status == PAGER_TRY_PIN_OK) {
            (void)pager_release_page_handle(&root_probe);
        }
        return fail(database,
                    owners,
                    owner_count,
                    "compact root was not evicted before SQL pressure check");
    }

    memset(&result, 0, sizeof(result));
    TinyDBSqlStatus status = tinydb_execute_sql(
        database,
        "SELECT * FROM compact WHERE id = 250;",
        &result);
    if (status != TINYDB_SQL_EXECUTE_ERROR ||
        result.status != TINYDB_SQL_EXECUTE_ERROR ||
        !result.statement_type_valid || result.statement_type != STATEMENT_SELECT ||
        result.executed || result.execute_result != EXECUTE_SUCCESS ||
        strstr(result.message, "buffer pool busy") == NULL) {
        return fail(database,
                    owners,
                    owner_count,
                    "primary-key SELECT did not fail closed with BUSY detail");
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        return fail(database,
                    owners,
                    owner_count,
                    "unable to release exactly one pressure owner");
    }

    memset(&result, 0, sizeof(result));
    status = tinydb_execute_sql(database,
                                "SELECT * FROM compact WHERE id = 250;",
                                &result);
    if (status != TINYDB_SQL_SUCCESS || result.status != TINYDB_SQL_SUCCESS ||
        !result.executed || result.execute_result != EXECUTE_SUCCESS) {
        return fail(database,
                    owners,
                    owner_count,
                    "primary-key SELECT did not recover with one free frame");
    }

    if (!release_handles(owners, owner_count)) {
        return fail(database, NULL, 0u, "unable to release pressure owners");
    }

    tinydb_close(database);
    printf("GENERIC_SQL_PIN_OK busy_nonfatal=yes root_evicted=yes one_free_frame_success=yes precise_message=yes\n");
    return 0;
}
