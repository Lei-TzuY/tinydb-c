#include "engine.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_migration.h"
#include "leaf_page_access.h"
#include "record.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_ROWS 24u
#define KEY_STEP 100u

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

static uint32_t read_u32_native(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool item_present(Table* table,
                         TableSchema* schema,
                         uint32_t id,
                         const char* expected_name,
                         uint32_t expected_price) {
    TinyDBRecord record;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_find(table, schema, id, &record) &&
           tinydb_record_decode(schema,
                                &record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                &count,
                                message,
                                sizeof(message)) &&
           count == 3u && values[0].int_value == id &&
           strcmp(values[1].text, expected_name) == 0 &&
           values[2].int_value == expected_price;
}

static bool item_absent(Table* table, TableSchema* schema, uint32_t id) {
    TinyDBRecord record;
    return !tinydb_record_find(table, schema, id, &record);
}

static bool migrate_leaf_containing(Table* table,
                                    TableSchema* schema,
                                    uint32_t key,
                                    uint32_t* page_num_out) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }
    uint32_t page_num = cursor->page_num;
    free(cursor);

    void* page = get_page(table->pager, page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
        TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        if (page_num_out != NULL) *page_num_out = page_num;
        table->root_page_num = previous_root;
        return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
    }
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
    pager_commit(table->pager);
    if (page_num_out != NULL) *page_num_out = page_num;
    table->root_page_num = previous_root;
    return true;
}

static bool migrate_first_two_leaves(Table* table,
                                     TableSchema* schema,
                                     uint32_t* left_page_num,
                                     uint32_t* next_page_num,
                                     uint32_t* left_max_key) {
    if (!migrate_leaf_containing(table,
                                 schema,
                                 KEY_STEP,
                                 left_page_num)) {
        return false;
    }

    unsigned char left[PAGE_SIZE];
    memcpy(left,
           get_page(table->pager, *left_page_num),
           sizeof(left));
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(left, PAGE_SIZE, &count) || count < 2u ||
        !tinydb_leaf_page_next(left, PAGE_SIZE, next_page_num) ||
        *next_page_num == 0u || *next_page_num >= table->pager->num_pages ||
        !tinydb_leaf_page_key_at(left,
                                 PAGE_SIZE,
                                 count - 1u,
                                 left_max_key)) {
        return false;
    }

    unsigned char next_before[PAGE_SIZE];
    memcpy(next_before,
           get_page(table->pager, *next_page_num),
           sizeof(next_before));
    uint32_t next_count = 0u;
    uint32_t next_first_key = 0u;
    if (!tinydb_leaf_page_count(next_before, PAGE_SIZE, &next_count) ||
        next_count == 0u ||
        !tinydb_leaf_page_key_at(next_before,
                                 PAGE_SIZE,
                                 0u,
                                 &next_first_key)) {
        return false;
    }

    uint32_t migrated_next = INVALID_PAGE_NUM;
    return migrate_leaf_containing(table,
                                   schema,
                                   next_first_key,
                                   &migrated_next) &&
           migrated_next == *next_page_num;
}

static bool parent_key_count(Table* table,
                             uint32_t child_page_num,
                             uint32_t* parent_page_num,
                             uint32_t* key_count) {
    if (child_page_num >= table->pager->num_pages || parent_page_num == NULL ||
        key_count == NULL) {
        return false;
    }
    unsigned char child[PAGE_SIZE];
    memcpy(child,
           get_page(table->pager, child_page_num),
           sizeof(child));
    *parent_page_num = read_u32_native(child + PARENT_POINTER_OFFSET);
    if (*parent_page_num == 0u ||
        *parent_page_num >= table->pager->num_pages) {
        return false;
    }
    unsigned char parent[PAGE_SIZE];
    memcpy(parent,
           get_page(table->pager, *parent_page_num),
           sizeof(parent));
    if (get_node_type(parent) != NODE_INTERNAL) return false;
    *key_count = read_u32_native(parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    return true;
}

static bool insert_candidate(TinyDB* db, uint32_t key) {
    char sql[192];
    snprintf(sql,
             sizeof(sql),
             "INSERT INTO items VALUES (%u, 'split-%u', %u);",
             key,
             key,
             key + 5000u);
    return exec_ok(db, sql);
}

static bool trigger_split(TinyDB* db,
                          Table* table,
                          TableSchema* schema,
                          uint32_t left_page_num,
                          uint32_t left_max_key,
                          uint32_t baseline_parent_keys,
                          uint32_t* split_key,
                          uint32_t* inserted_count) {
    uint32_t count = 0u;
    for (uint32_t key = KEY_STEP + 1u; key < left_max_key; key++) {
        if (key % KEY_STEP == 0u) continue;
        if (!insert_candidate(db, key)) return false;
        count++;

        uint32_t parent_page = 0u;
        uint32_t parent_keys = 0u;
        if (!parent_key_count(table,
                              left_page_num,
                              &parent_page,
                              &parent_keys)) {
            return false;
        }
        if (parent_keys > baseline_parent_keys) {
            char expected_name[64];
            snprintf(expected_name, sizeof(expected_name), "split-%u", key);
            if (!item_present(table,
                              schema,
                              key,
                              expected_name,
                              key + 5000u)) {
                return false;
            }
            *split_key = key;
            *inserted_count = count;
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }
    remove(argv[1]);

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL ||
        !exec_ok(db,
                 "CREATE TABLE items (id INT, name VARCHAR(32), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return EXIT_FAILURE;
    }

    for (uint32_t i = 1u; i <= INITIAL_ROWS; i++) {
        uint32_t key = i * KEY_STEP;
        char sql[192];
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO items VALUES (%u, 'seed-%u', %u);",
                 key,
                 key,
                 key * 10u);
        if (!exec_ok(db, sql)) {
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }

    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t left_page_num = INVALID_PAGE_NUM;
    uint32_t next_page_num = INVALID_PAGE_NUM;
    uint32_t left_max_key = 0u;
    if (schema == NULL || schema->row_size != 41u ||
        !migrate_first_two_leaves(table,
                                  schema,
                                  &left_page_num,
                                  &next_page_num,
                                  &left_max_key)) {
        fprintf(stderr, "unable to prepare adjacent V2 leaves\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t parent_page_num = 0u;
    uint32_t baseline_parent_keys = 0u;
    if (!parent_key_count(table,
                          left_page_num,
                          &parent_page_num,
                          &baseline_parent_keys) ||
        tinydb_record_scan(table, schema, NULL, NULL) != INITIAL_ROWS) {
        fprintf(stderr, "invalid baseline tree before split INSERT\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    /* First force the live split inside a user transaction. All non-split
     * inserts leading up to the overflow and all four split page images must
     * disappear together on ROLLBACK. */
    uint32_t rollback_split_key = 0u;
    uint32_t rollback_inserted = 0u;
    if (!exec_ok(db, "BEGIN;") ||
        !trigger_split(db,
                       table,
                       schema,
                       left_page_num,
                       left_max_key,
                       baseline_parent_keys,
                       &rollback_split_key,
                       &rollback_inserted) ||
        rollback_inserted == 0u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional V2 split INSERT could not be staged/rolled back\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t rolled_back_parent = 0u;
    uint32_t rolled_back_parent_keys = 0u;
    if (!parent_key_count(table,
                          left_page_num,
                          &rolled_back_parent,
                          &rolled_back_parent_keys) ||
        rolled_back_parent != parent_page_num ||
        rolled_back_parent_keys != baseline_parent_keys ||
        tinydb_record_scan(table, schema, NULL, NULL) != INITIAL_ROWS ||
        !item_absent(table, schema, rollback_split_key) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "V2 split INSERT rollback leaked topology or rows\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    /* Repeat the exact workload in autocommit mode. The first parent key-count
     * increase proves the production insert path allocated and published a new
     * V2 leaf rather than merely using the non-split fast path. */
    uint32_t committed_split_key = 0u;
    uint32_t committed_inserted = 0u;
    if (!trigger_split(db,
                       table,
                       schema,
                       left_page_num,
                       left_max_key,
                       baseline_parent_keys,
                       &committed_split_key,
                       &committed_inserted) ||
        committed_inserted != rollback_inserted ||
        committed_split_key != rollback_split_key ||
        tinydb_record_scan(table, schema, NULL, NULL) !=
            INITIAL_ROWS + committed_inserted ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed production V2 split INSERT failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t committed_parent = 0u;
    uint32_t committed_parent_keys = 0u;
    if (!parent_key_count(table,
                          left_page_num,
                          &committed_parent,
                          &committed_parent_keys) ||
        committed_parent != parent_page_num ||
        committed_parent_keys != baseline_parent_keys + 1u) {
        fprintf(stderr, "production V2 split did not publish parent topology\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    char expected_name[64];
    snprintf(expected_name,
             sizeof(expected_name),
             "split-%u",
             committed_split_key);
    if (schema == NULL ||
        tinydb_record_scan(table, schema, NULL, NULL) !=
            INITIAL_ROWS + committed_inserted ||
        !item_present(table,
                      schema,
                      committed_split_key,
                      expected_name,
                      committed_split_key + 5000u) ||
        !parent_key_count(table,
                          left_page_num,
                          &committed_parent,
                          &committed_parent_keys) ||
        committed_parent_keys != baseline_parent_keys + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "production V2 split INSERT did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("SLOTTED_V2_SPLIT_INSERT_OK split_key=%u inserts=%u rollback=yes commit=yes reopen=yes parent_update=yes wal=yes\n",
           committed_split_key,
           committed_inserted);
    tinydb_close(db);
    remove(argv[1]);
    return EXIT_SUCCESS;
}
