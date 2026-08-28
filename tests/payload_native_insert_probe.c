#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "record_payload.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_payloads");
    schema.root_page_num = root_page_num;
    schema.num_columns = 3u;

    snprintf(schema.columns[0].name, sizeof(schema.columns[0].name), "%s", "id");
    schema.columns[0].type = COL_TYPE_INT;
    schema.columns[0].offset = 0u;
    schema.columns[0].size = 4u;

    snprintf(schema.columns[1].name, sizeof(schema.columns[1].name), "%s", "description");
    schema.columns[1].type = COL_TYPE_VARCHAR;
    schema.columns[1].offset = 4u;
    schema.columns[1].size = 300u;

    snprintf(schema.columns[2].name, sizeof(schema.columns[2].name), "%s", "score");
    schema.columns[2].type = COL_TYPE_INT;
    schema.columns[2].offset = 304u;
    schema.columns[2].size = 4u;
    schema.row_size = 308u;
    return schema;
}

static uint32_t read_u32(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool make_payload(const TableSchema* schema,
                         uint32_t id,
                         const char* description,
                         uint32_t score,
                         TinyDBRecordPayload* payload) {
    TinyDBValue values[3];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", description);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = score;
    return tinydb_record_payload_encode_values(schema,
                                               values,
                                               3u,
                                               payload,
                                               message,
                                               sizeof(message));
}

static bool insert_payload(Table* table,
                           const TableSchema* schema,
                           uint32_t id,
                           const char* description,
                           uint32_t score,
                           char* message,
                           size_t message_size) {
    TinyDBRecordPayload payload;
    return make_payload(schema, id, description, score, &payload) &&
           tinydb_record_payload_insert(table,
                                        schema,
                                        &payload,
                                        message,
                                        message_size);
}

static bool expect_payload(Table* table,
                           const TableSchema* schema,
                           uint32_t id,
                           const char* description,
                           uint32_t score) {
    TinyDBRecordPayload payload;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_find(table,
                                    schema,
                                    id,
                                    &payload,
                                    message,
                                    sizeof(message)) ||
        !tinydb_record_payload_decode_values(schema,
                                             &payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message))) {
        fprintf(stderr, "payload lookup/decode failed for %u: %s\n", id, message);
        return false;
    }
    return value_count == 3u &&
           values[0].int_value == id &&
           strcmp(values[1].text, description) == 0 &&
           values[2].int_value == score;
}

static bool payload_missing(Table* table,
                            const TableSchema* schema,
                            uint32_t id) {
    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return !tinydb_record_payload_find(table,
                                       schema,
                                       id,
                                       &payload,
                                       message,
                                       sizeof(message));
}

static uint32_t payload_count(Table* table, const TableSchema* schema) {
    bool complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    uint32_t count = tinydb_record_payload_scan(table,
                                                schema,
                                                NULL,
                                                NULL,
                                                &complete,
                                                message,
                                                sizeof(message));
    return complete ? count : UINT32_MAX;
}

static bool root_has_two_v2_leaf_children(Table* table,
                                          const TableSchema* schema) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           PAGE_SIZE);
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return false;
    }

    uint32_t left_page_num = read_u32(root + INTERNAL_NODE_HEADER_SIZE);
    uint32_t separator = read_u32(root + INTERNAL_NODE_HEADER_SIZE +
                                  INTERNAL_NODE_CHILD_SIZE);
    uint32_t right_page_num = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    if (left_page_num == 0u || right_page_num == 0u ||
        left_page_num == right_page_num ||
        left_page_num >= table->pager->num_pages ||
        right_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    memcpy(left, get_page(table->pager, left_page_num), PAGE_SIZE);
    memcpy(right, get_page(table->pager, right_page_num), PAGE_SIZE);
    uint32_t left_count = 0u;
    uint32_t right_count = 0u;
    uint32_t left_max = 0u;
    uint32_t right_min = 0u;
    uint32_t left_prev = INVALID_PAGE_NUM;
    uint32_t left_next = INVALID_PAGE_NUM;
    uint32_t right_prev = INVALID_PAGE_NUM;
    uint32_t right_next = INVALID_PAGE_NUM;

    return tinydb_leaf_format_detect_page(left, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_leaf_format_detect_page(right, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) &&
           tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE) &&
           left[IS_ROOT_OFFSET] == 0u && right[IS_ROOT_OFFSET] == 0u &&
           read_u32(left + PARENT_POINTER_OFFSET) == schema->root_page_num &&
           read_u32(right + PARENT_POINTER_OFFSET) == schema->root_page_num &&
           tinydb_leaf_page_count(left, PAGE_SIZE, &left_count) &&
           tinydb_leaf_page_count(right, PAGE_SIZE, &right_count) &&
           left_count > 0u && right_count > 0u &&
           tinydb_leaf_page_key_at(left, PAGE_SIZE, left_count - 1u, &left_max) &&
           tinydb_leaf_page_key_at(right, PAGE_SIZE, 0u, &right_min) &&
           separator == left_max && left_max < right_min &&
           tinydb_leaf_page_prev(left, PAGE_SIZE, &left_prev) && left_prev == 0u &&
           tinydb_leaf_page_next(left, PAGE_SIZE, &left_next) &&
           left_next == right_page_num &&
           tinydb_leaf_page_prev(right, PAGE_SIZE, &right_prev) &&
           right_prev == left_page_num &&
           tinydb_leaf_page_next(right, PAGE_SIZE, &right_next) && right_next == 0u;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_native_insert_probe <db>\n");
        return 1;
    }

    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return 1;

    uint32_t root = table->root_page_num;
    void* page = get_page(table->pager, root);
    TableSchema schema = wide_schema(root);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(&schema, message, sizeof(message)) ||
        schema.row_size <= ROW_SIZE ||
        !tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) {
        fprintf(stderr, "wide schema/V2 initialization failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }
    set_node_root(page, true);

    if (!insert_payload(table, &schema, 10u, "ten", 100u, message, sizeof(message)) ||
        !insert_payload(table, &schema, 5u, "five", 50u, message, sizeof(message)) ||
        !insert_payload(table, &schema, 20u, "twenty", 200u, message, sizeof(message)) ||
        payload_count(table, &schema) != 3u ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "payload-native root-leaf insertion failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (insert_payload(table,
                       &schema,
                       10u,
                       "duplicate must not publish",
                       999u,
                       message,
                       sizeof(message)) ||
        strstr(message, "duplicate primary key") == NULL ||
        payload_count(table, &schema) != 3u ||
        !expect_payload(table, &schema, 10u, "ten", 100u)) {
        fprintf(stderr, "duplicate insert was not fail-closed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    memset(long_text, 'w', TINYDB_RECORD_TEXT_MAX - 1u);
    long_text[TINYDB_RECORD_TEXT_MAX - 1u] = '\0';

    uint32_t split_key = 0u;
    uint32_t next_id = 1000u;
    while (get_node_type(get_page(table->pager, schema.root_page_num)) == NODE_LEAF) {
        if (!insert_payload(table,
                            &schema,
                            next_id,
                            long_text,
                            next_id + 1u,
                            message,
                            sizeof(message))) {
            fprintf(stderr, "payload-native root split trigger failed: %s\n", message);
            db_close(table);
            remove(argv[1]);
            return 1;
        }
        split_key = next_id;
        if (next_id > UINT32_MAX - 10u) {
            db_close(table);
            remove(argv[1]);
            return 1;
        }
        next_id += 10u;
    }

    uint32_t count_after_split = payload_count(table, &schema);
    if (split_key == 0u || count_after_split == UINT32_MAX ||
        count_after_split <= 3u ||
        !root_has_two_v2_leaf_children(table, &schema) ||
        !expect_payload(table, &schema, split_key, long_text, split_key + 1u) ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "payload-native root split topology/data check failed\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    /* The next increasing key targets the right child and would raise its
     * subtree maximum. The payload-native non-root separator path is still
     * deliberately fail-closed, and failure must leave the split tree intact. */
    uint32_t unsupported_key = next_id;
    if (insert_payload(table,
                       &schema,
                       unsupported_key,
                       long_text,
                       unsupported_key + 1u,
                       message,
                       sizeof(message)) ||
        strstr(message, "topology-neutral") == NULL ||
        payload_count(table, &schema) != count_after_split ||
        !payload_missing(table, &schema, unsupported_key) ||
        !root_has_two_v2_leaf_children(table, &schema) ||
        !expect_payload(table, &schema, split_key, long_text, split_key + 1u)) {
        fprintf(stderr, "post-split unsupported boundary insert was not atomic: %s\n",
                message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) {
        remove(argv[1]);
        return 1;
    }
    schema = wide_schema(table->root_page_num);
    if (payload_count(table, &schema) != count_after_split ||
        !root_has_two_v2_leaf_children(table, &schema) ||
        !payload_missing(table, &schema, unsupported_key) ||
        !expect_payload(table, &schema, split_key, long_text, split_key + 1u) ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "reopen lost payload-native root split\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    TinyDBRecord legacy;
    if (tinydb_record_find(table, &schema, 10u, &legacy)) {
        fprintf(stderr, "legacy TinyDBRecord unexpectedly accepted 308-byte inserted row\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    remove(argv[1]);
    printf("PAYLOAD_NATIVE_INSERT_OK row_size=308 initial_root=1 duplicate_guard=1 root_split=1 nonroot_guard=1 reopen=1 rows=%u split_key=%u\n",
           count_after_split,
           split_key);
    return 0;
}
