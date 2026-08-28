#include "engine.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEAF_COUNT 4u
#define BASELINE_ROWS 4u

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

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void make_values(uint32_t id, TinyDBValue values[3]) {
    memset(values, 0, sizeof(TinyDBValue) * 3u);
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "boundary-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 1000u;
}

static bool raw_insert(const TableSchema* schema,
                       unsigned char page[PAGE_SIZE],
                       uint32_t id) {
    TinyDBValue values[3];
    TinyDBRecord record;
    TinyDBRecordPayload payload;
    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    make_values(id, values);
    return tinydb_record_encode(schema,
                                values,
                                3u,
                                &record,
                                message,
                                sizeof(message)) &&
           tinydb_record_payload_from_record(schema,
                                             &record,
                                             &payload,
                                             message,
                                             sizeof(message)) &&
           tinydb_row_envelope_encode_compact_v2(schema,
                                                 &payload,
                                                 envelope,
                                                 PAGE_SIZE,
                                                 &envelope_length) &&
           envelope_length > 0u && envelope_length <= UINT16_MAX &&
           tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         id,
                                         envelope,
                                         (uint16_t)envelope_length);
}

static bool seed_tree(TinyDB* db,
                      TableSchema* schema,
                      uint32_t leaf_pages[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_page_num = schema->root_page_num;
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u, 40u};

    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        leaf_pages[i] = get_unused_page_num(pager);
        if (leaf_pages[i] == 0u || leaf_pages[i] == INVALID_PAGE_NUM) return false;
        unsigned char* leaf = (unsigned char*)get_page(pager, leaf_pages[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, root_page_num);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaf_pages[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == LEAF_COUNT ? 0u : leaf_pages[i + 1u]);
        if (!raw_insert(schema, leaf, keys[i]) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) {
            return false;
        }
        mark_page_dirty(pager, leaf_pages[i]);
    }

    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    memset(root, 0, PAGE_USABLE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, LEAF_COUNT - 1u);
    for (uint32_t i = 0u; i + 1u < LEAF_COUNT; i++) {
        unsigned char* cell =
            root + INTERNAL_NODE_HEADER_SIZE + i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, leaf_pages[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, keys[i]);
    }
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              leaf_pages[LEAF_COUNT - 1u]);
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE)) return false;
    mark_page_dirty(pager, root_page_num);
    pager_commit(pager);

    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_matches(Table* table,
                         const TableSchema* schema,
                         const uint32_t* children,
                         const uint32_t* separators,
                         uint32_t child_count) {
    if (child_count < 2u) return false;
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE) ||
        root[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            child_count - 1u) {
        return false;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        if (tinydb_parent_stage_child_at(root, i) != children[i]) return false;
        if (i + 1u < child_count &&
            tinydb_parent_stage_key_at(root, i) != separators[i]) {
            return false;
        }
    }
    return true;
}

static bool leaf_links(Table* table,
                       uint32_t page_num,
                       uint32_t expected_prev,
                       uint32_t expected_next) {
    unsigned char leaf[PAGE_SIZE];
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    memcpy(leaf, get_page(table->pager, page_num), sizeof(leaf));
    return tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE) &&
           tinydb_leaf_page_prev(leaf, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(leaf, PAGE_SIZE, &next) &&
           prev == expected_prev && next == expected_next;
}

static bool free_list_contains(const Pager* pager, uint32_t page_num) {
    if (pager == NULL) return false;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static bool record_present(Table* table,
                           const TableSchema* schema,
                           uint32_t key) {
    TinyDBRecord record;
    return tinydb_record_find(table, schema, key, &record);
}

static bool delete_key(Table* table,
                       const TableSchema* schema,
                       uint32_t key,
                       char message[TINYDB_RECORD_MESSAGE_MAX]) {
    message[0] = '\0';
    return tinydb_record_delete(table,
                                schema,
                                key,
                                message,
                                TINYDB_RECORD_MESSAGE_MAX);
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
                 "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return EXIT_FAILURE;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t leaves[LEAF_COUNT] = {0u, 0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, leaves)) {
        fprintf(stderr, "unable to seed boundary empty-leaf tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    const uint32_t original_children[4] = {
        leaves[0], leaves[1], leaves[2], leaves[3]
    };
    const uint32_t original_separators[3] = {10u, 20u, 30u};
    uint32_t baseline_free_count = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!root_matches(table,
                      schema,
                      original_children,
                      original_separators,
                      4u) ||
        !exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 10u, message)) {
        fprintf(stderr, "left-boundary transactional delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    const uint32_t after_left_children[3] = {leaves[1], leaves[2], leaves[3]};
    const uint32_t after_left_separators[2] = {20u, 30u};
    if (record_present(table, schema, 10u) ||
        !root_matches(table,
                      schema,
                      after_left_children,
                      after_left_separators,
                      3u) ||
        !leaf_links(table, leaves[1], 0u, leaves[2]) ||
        table->pager->free_page_count != baseline_free_count + 1u ||
        !free_list_contains(table->pager, leaves[0]) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "left-boundary transaction topology mismatch\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    if (!record_present(table, schema, 10u) ||
        !root_matches(table,
                      schema,
                      original_children,
                      original_separators,
                      4u) ||
        !leaf_links(table, leaves[0], 0u, leaves[1]) ||
        !leaf_links(table, leaves[1], leaves[0], leaves[2]) ||
        table->pager->free_page_count != baseline_free_count ||
        free_list_contains(table->pager, leaves[0]) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "left-boundary rollback leaked topology or free state\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 10u, message) ||
        record_present(table, schema, 10u) ||
        !root_matches(table,
                      schema,
                      after_left_children,
                      after_left_separators,
                      3u) ||
        !leaf_links(table, leaves[1], 0u, leaves[2]) ||
        !free_list_contains(table->pager, leaves[0]) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed left-boundary delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t after_left_free_count = table->pager->free_page_count;
    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 40u, message)) {
        fprintf(stderr, "right-boundary transactional delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    const uint32_t middle_children[2] = {leaves[1], leaves[2]};
    const uint32_t middle_separators[1] = {20u};
    if (record_present(table, schema, 40u) ||
        !root_matches(table,
                      schema,
                      middle_children,
                      middle_separators,
                      2u) ||
        !leaf_links(table, leaves[2], leaves[1], 0u) ||
        table->pager->free_page_count != after_left_free_count + 1u ||
        !free_list_contains(table->pager, leaves[3]) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "right-boundary transaction topology mismatch\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    if (!record_present(table, schema, 40u) ||
        !root_matches(table,
                      schema,
                      after_left_children,
                      after_left_separators,
                      3u) ||
        !leaf_links(table, leaves[3], leaves[2], 0u) ||
        table->pager->free_page_count != after_left_free_count ||
        free_list_contains(table->pager, leaves[3]) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "right-boundary rollback leaked topology or free state\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 40u, message) ||
        record_present(table, schema, 40u) ||
        !root_matches(table,
                      schema,
                      middle_children,
                      middle_separators,
                      2u) ||
        !leaf_links(table, leaves[1], 0u, leaves[2]) ||
        !leaf_links(table, leaves[2], leaves[1], 0u) ||
        !free_list_contains(table->pager, leaves[0]) ||
        !free_list_contains(table->pager, leaves[3]) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed right-boundary delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (delete_key(table, schema, 30u, message) ||
        strstr(message, "underflow") == NULL ||
        !record_present(table, schema, 30u) ||
        !root_matches(table,
                      schema,
                      middle_children,
                      middle_separators,
                      2u) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "two-child parent underflow did not remain fail-closed: %s\n",
                message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        record_present(table, schema, 10u) ||
        record_present(table, schema, 40u) ||
        !record_present(table, schema, 20u) ||
        !record_present(table, schema, 30u) ||
        !root_matches(table,
                      schema,
                      middle_children,
                      middle_separators,
                      2u) ||
        !leaf_links(table, leaves[1], 0u, leaves[2]) ||
        !leaf_links(table, leaves[2], leaves[1], 0u) ||
        !free_list_contains(table->pager, leaves[0]) ||
        !free_list_contains(table->pager, leaves[3]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 2u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "boundary empty-leaf removal did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("V2_EMPTY_LEAF_BOUNDARY_OK left=yes right=yes rollback=yes "
           "allocator=yes underflow_guard=yes reopen=yes integrity=yes wal=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
