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

    /* The API itself creates the first row in an empty V2 root. Then exercise
     * both lower-boundary and new-maximum insertion while the root stays a
     * single topology-neutral leaf. */
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

    uint32_t next_id = 1000u;
    while (insert_payload(table,
                          &schema,
                          next_id,
                          long_text,
                          next_id + 1u,
                          message,
                          sizeof(message))) {
        next_id++;
        if (next_id > 2000u) {
            fprintf(stderr, "payload-native insert never reached leaf capacity\n");
            db_close(table);
            remove(argv[1]);
            return 1;
        }
    }

    uint32_t count_after_full = payload_count(table, &schema);
    if (count_after_full == UINT32_MAX || count_after_full <= 3u ||
        strstr(message, "cannot split") == NULL ||
        !payload_missing(table, &schema, next_id) ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "capacity failure was not atomic: %s\n", message);
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
    if (payload_count(table, &schema) != count_after_full ||
        !payload_missing(table, &schema, next_id) ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "reopen lost payload-native inserts\n");
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
    printf("PAYLOAD_NATIVE_INSERT_OK row_size=308 initial_root=1 duplicate_guard=1 capacity_guard=1 reopen=1 rows=%u\n",
           count_after_full);
    return 0;
}
