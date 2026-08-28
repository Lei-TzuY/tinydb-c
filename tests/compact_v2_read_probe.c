#include "engine.h"
#include "leaf_format.h"
#include "leaf_migration.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    if (tinydb_execute_sql(db, sql, &result) != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool expect_item(Table* table,
                        TableSchema* schema,
                        uint32_t id,
                        const char* name,
                        uint32_t price) {
    TinyDBRecord record;
    if (!tinydb_record_find(table, schema, id, &record)) return false;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_decode(schema,
                                &record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                &count,
                                message,
                                sizeof(message)) &&
           count == 3u &&
           values[0].int_value == id &&
           strcmp(values[1].text, name) == 0 &&
           values[2].int_value == price;
}

static bool install_compact_row(Table* table, TableSchema* schema, uint32_t key) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = table_find(table, key);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        return false;
    }
    uint32_t page_num = cursor->page_num;
    free(cursor);

    void* page = get_page(table->pager, page_num);
    if (!tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)) {
        table->root_page_num = previous_root;
        return false;
    }
    unsigned char migrated[PAGE_SIZE];
    memset(migrated, 0, sizeof(migrated));
    if (!tinydb_leaf_migrate_v1_to_v2(page,
                                      PAGE_SIZE,
                                      schema->row_size,
                                      migrated,
                                      sizeof(migrated))) {
        table->root_page_num = previous_root;
        return false;
    }
    memcpy(page, migrated, PAGE_USABLE_SIZE);
    mark_page_dirty(table->pager, page_num);

    Cursor* v2_cursor = tinydb_leaf_read_find(table, key);
    if (v2_cursor == NULL || v2_cursor->page_num != page_num) {
        free(v2_cursor);
        table->root_page_num = previous_root;
        return false;
    }
    const void* raw = NULL;
    uint32_t raw_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   v2_cursor->cell_num,
                                   &raw,
                                   &raw_length) ||
        raw_length != schema->row_size) {
        free(v2_cursor);
        table->root_page_num = previous_root;
        return false;
    }

    TinyDBRecordPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.length = raw_length;
    memcpy(payload.bytes, raw, raw_length);
    unsigned char compact[512];
    uint32_t compact_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               compact,
                                               sizeof(compact),
                                               &compact_length) ||
        compact_length >= TINYDB_ROW_ENVELOPE_HEADER_SIZE + schema->row_size) {
        free(v2_cursor);
        table->root_page_num = previous_root;
        return false;
    }

    bool updated = tinydb_slotted_leaf_v2_update(page,
                                                 PAGE_SIZE,
                                                 key,
                                                 compact,
                                                 (uint16_t)compact_length);
    free(v2_cursor);
    if (updated) {
        mark_page_dirty(table->pager, page_num);
        pager_commit(table->pager);
    }
    table->root_page_num = previous_root;
    return updated;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: compact_v2_read_probe <db>\n");
        return 1;
    }
    remove(argv[1]);

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL) return 1;
    if (!exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(64), price INT);")) {
        tinydb_close(db);
        return 1;
    }
    for (uint32_t i = 1u; i <= 24u; i++) {
        char sql[192];
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO items VALUES (%u, 'item-%u', %u);",
                 i,
                 i,
                 i * 10u);
        if (!exec_ok(db, sql)) {
            tinydb_close(db);
            return 1;
        }
    }

    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    if (schema == NULL || schema->row_size != 73u ||
        !install_compact_row(table, schema, 1u) ||
        !expect_item(table, schema, 1u, "item-1", 10u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 24u) {
        fprintf(stderr, "compact V2 row was not readable before reopen\n");
        tinydb_close(db);
        return 1;
    }
    tinydb_close(db);

    db = tinydb_open(argv[1]);
    if (db == NULL) return 1;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !expect_item(table, schema, 1u, "item-1", 10u) ||
        !expect_item(table, schema, 12u, "item-12", 120u) ||
        !expect_item(table, schema, 24u, "item-24", 240u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 24u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "compact V2 row did not survive production reopen/read\n");
        tinydb_close(db);
        return 1;
    }
    tinydb_close(db);
    remove(argv[1]);
    printf("PASS: production generic lookup/scan reopens a mixed tree containing a compact V2 VARCHAR row while preserving legacy fixed rows.\n");
    return 0;
}
