#include "engine.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHILD_COUNT 3u
#define BASELINE_ROWS 3u

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

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void make_values(uint32_t id, TinyDBValue values[3]) {
    memset(values, 0, sizeof(TinyDBValue) * 3u);
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "leaf-%u", id);
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

static bool initialize_leaf(Pager* pager,
                            uint32_t page_num,
                            uint32_t parent_page_num,
                            uint32_t previous_page_num,
                            uint32_t next_page_num) {
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        previous_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        next_page_num);
    return true;
}

static bool seed_tree(TinyDB* db,
                      TableSchema* schema,
                      uint32_t leaf_pages[CHILD_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_page_num = schema->root_page_num;

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        leaf_pages[i] = get_unused_page_num(pager);
        if (leaf_pages[i] == 0u || leaf_pages[i] == INVALID_PAGE_NUM) {
            return false;
        }
        (void)get_page(pager, leaf_pages[i]);
    }

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        if (!initialize_leaf(pager,
                             leaf_pages[i],
                             root_page_num,
                             i == 0u ? 0u : leaf_pages[i - 1u],
                             i + 1u == CHILD_COUNT ? 0u : leaf_pages[i + 1u]) ||
            !raw_insert(schema,
                        (unsigned char*)get_page(pager, leaf_pages[i]),
                        10u * (i + 1u))) {
            return false;
        }
        mark_page_dirty(pager, leaf_pages[i]);
    }

    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    memset(root, 0, PAGE_USABLE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, 2u);
    unsigned char* cell0 = root + INTERNAL_NODE_HEADER_SIZE;
    unsigned char* cell1 = cell0 + INTERNAL_NODE_CELL_SIZE;
    write_u32(cell0, leaf_pages[0]);
    write_u32(cell0 + INTERNAL_NODE_CHILD_SIZE, 10u);
    write_u32(cell1, leaf_pages[1]);
    write_u32(cell1 + INTERNAL_NODE_CHILD_SIZE, 20u);
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, leaf_pages[2]);
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE)) return false;
    mark_page_dirty(pager, root_page_num);
    pager_commit(pager);

    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_three_children(Table* table,
                                const TableSchema* schema,
                                const uint32_t leaf_pages[CHILD_COUNT]) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 2u &&
           tinydb_parent_stage_child_at(root, 0u) == leaf_pages[0] &&
           tinydb_parent_stage_child_at(root, 1u) == leaf_pages[1] &&
           tinydb_parent_stage_child_at(root, 2u) == leaf_pages[2] &&
           tinydb_parent_stage_key_at(root, 0u) == 10u &&
           tinydb_parent_stage_key_at(root, 1u) == 20u;
}

static bool root_two_children(Table* table,
                              const TableSchema* schema,
                              const uint32_t leaf_pages[CHILD_COUNT]) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u &&
           tinydb_parent_stage_child_at(root, 0u) == leaf_pages[0] &&
           tinydb_parent_stage_child_at(root, 1u) == leaf_pages[2] &&
           tinydb_parent_stage_key_at(root, 0u) == 10u;
}

static bool leaf_links(Table* table,
                       uint32_t page_num,
                       uint32_t expected_previous,
                       uint32_t expected_next) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), sizeof(page));
    uint32_t previous_page_num = UINT32_MAX;
    uint32_t next_page_num = UINT32_MAX;
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &previous_page_num) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next_page_num) &&
           previous_page_num == expected_previous &&
           next_page_num == expected_next;
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

static bool underflow_fail_closed_message(const char* message) {
    return message != NULL && strstr(message, "underflow") != NULL;
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
    uint32_t leaf_pages[CHILD_COUNT] = {0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, leaf_pages) ||
        !root_three_children(table, schema, leaf_pages) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[1]) ||
        !leaf_links(table, leaf_pages[1], leaf_pages[0], leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[1], 0u)) {
        fprintf(stderr, "unable to seed V2 empty-leaf removal tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 20u, message) ||
        record_present(table, schema, 20u) ||
        !root_two_children(table, schema, leaf_pages) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[0], 0u) ||
        table->pager->free_page_count != free_before + 1u ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional empty-leaf removal failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!record_present(table, schema, 20u) ||
        !root_three_children(table, schema, leaf_pages) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[1]) ||
        !leaf_links(table, leaf_pages[1], leaf_pages[0], leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[1], 0u) ||
        table->pager->free_page_count != free_before ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "empty-leaf rollback did not restore topology\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 20u, message) ||
        record_present(table, schema, 20u) ||
        !root_two_children(table, schema, leaf_pages) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[0], 0u) ||
        table->pager->free_page_count != free_before + 1u ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed empty-leaf removal failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    bool left_deleted = delete_key(table, schema, 10u, message);
    bool left_message_ok = underflow_fail_closed_message(message);
    bool left_record_ok = record_present(table, schema, 10u);
    bool left_root_ok = root_two_children(table, schema, leaf_pages);
    bool left_free_ok = table->pager->free_page_count == free_before + 1u;
    uint32_t left_scan_count = tinydb_record_scan(table, schema, NULL, NULL);
    bool left_integrity_ok = exec_ok(db, "PRAGMA integrity_check;");
    if (left_deleted || !left_message_ok || !left_record_ok || !left_root_ok ||
        !left_free_ok || left_scan_count != BASELINE_ROWS - 1u ||
        !left_integrity_ok) {
        fprintf(stderr,
                "root-collapse fail-closed mismatch: deleted=%s message_ok=%s "
                "record_ok=%s root_ok=%s free_ok=%s scan=%u integrity=%s "
                "message=%s\n",
                left_deleted ? "yes" : "no",
                left_message_ok ? "yes" : "no",
                left_record_ok ? "yes" : "no",
                left_root_ok ? "yes" : "no",
                left_free_ok ? "yes" : "no",
                left_scan_count,
                left_integrity_ok ? "yes" : "no",
                message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    bool right_deleted = delete_key(table, schema, 30u, message);
    bool right_message_ok = underflow_fail_closed_message(message);
    bool right_record_ok = record_present(table, schema, 30u);
    bool right_root_ok = root_two_children(table, schema, leaf_pages);
    bool right_free_ok = table->pager->free_page_count == free_before + 1u;
    bool right_integrity_ok = exec_ok(db, "PRAGMA integrity_check;");
    if (right_deleted || !right_message_ok || !right_record_ok ||
        !right_root_ok || !right_free_ok || !right_integrity_ok) {
        fprintf(stderr,
                "two-child underflow fail-closed mismatch: deleted=%s "
                "message_ok=%s record_ok=%s root_ok=%s free_ok=%s "
                "integrity=%s message=%s\n",
                right_deleted ? "yes" : "no",
                right_message_ok ? "yes" : "no",
                right_record_ok ? "yes" : "no",
                right_root_ok ? "yes" : "no",
                right_free_ok ? "yes" : "no",
                right_integrity_ok ? "yes" : "no",
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
        record_present(table, schema, 20u) ||
        !record_present(table, schema, 10u) ||
        !record_present(table, schema, 30u) ||
        !root_two_children(table, schema, leaf_pages) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[0], 0u) ||
        table->pager->free_page_count != free_before + 1u ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "empty-leaf removal state did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("V2_EMPTY_LEAF_DELETE_OK interior_child_remove=yes sibling_relink=yes "
           "rollback=yes page_reclaim=yes root_collapse_fail_closed=yes "
           "underflow_fail_closed=yes reopen=yes integrity=yes wal=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
