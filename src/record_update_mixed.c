#include "record_update_mixed.h"

#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static void begin_root_scope(Table* table,
                             const TableSchema* schema,
                             uint32_t* previous_root) {
    *previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
}

static void end_root_scope(Table* table, uint32_t previous_root) {
    table->root_page_num = previous_root;
}

static bool cursor_matches_key(Cursor* cursor, uint32_t key) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    uint32_t found = 0u;
    return get_node_type(page) == NODE_LEAF &&
           tinydb_leaf_page_key_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &found) &&
           found == key;
}

static bool write_fixed_v1(Cursor* cursor,
                           const TinyDBRecordPayload* payload) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE) ||
        !tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &stored_length) ||
        value == NULL || stored_length != ROW_SIZE ||
        payload->length > stored_length) {
        return false;
    }

    void* writable = (void*)value;
    memset(writable, 0, stored_length);
    memcpy(writable, payload->bytes, payload->length);
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    return true;
}

static bool write_slotted_v2(Cursor* cursor,
                             const TableSchema* schema,
                             uint32_t key,
                             const TinyDBRecordPayload* payload) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
        TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return false;
    }

    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               payload,
                                               envelope,
                                               sizeof(envelope),
                                               &envelope_length) ||
        envelope_length == 0u || envelope_length > UINT16_MAX) {
        return false;
    }
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length)) {
        return false;
    }
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    return true;
}

bool tinydb_record_update_existing_mixed(
    Table* table,
    const TableSchema* schema,
    uint32_t id,
    const TinyDBValue* values,
    uint32_t value_count,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required");
        return false;
    }
    if (!tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }
    if (ci_equal(schema->name, "users")) {
        set_message(message,
                    message_size,
                    "use the legacy users execution path so its secondary indexes stay synchronized");
        return false;
    }

    TinyDBRecord record;
    if (!tinydb_record_encode(schema,
                              values,
                              value_count,
                              &record,
                              message,
                              message_size)) {
        return false;
    }
    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_from_record(schema,
                                           &record,
                                           &payload,
                                           message,
                                           message_size)) {
        return false;
    }

    uint32_t encoded_id = 0u;
    memcpy(&encoded_id, payload.bytes, sizeof(encoded_id));
    if (encoded_id != id) {
        set_message(message,
                    message_size,
                    "generic UPDATE cannot change the primary-key id");
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    if (!cursor_matches_key(cursor, id)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found");
        return false;
    }

    void* page = get_page(table->pager, cursor->page_num);
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    bool updated = false;
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        updated = write_fixed_v1(cursor, &payload);
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        updated = write_slotted_v2(cursor, schema, id, &payload);
    }

    free(cursor);
    end_root_scope(table, previous_root);
    if (!updated) {
        set_message(message,
                    message_size,
                    format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2
                        ? "unable to update compact slotted V2 row without splitting the leaf"
                        : "unable to update existing logical leaf value");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
