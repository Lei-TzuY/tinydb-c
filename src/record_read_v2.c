#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "record_payload.h"
#include "row_envelope.h"

#include <stdlib.h>
#include <string.h>

static void begin_root_scope(Table* table,
                             const TableSchema* schema,
                             uint32_t* previous_root) {
    *previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
}

static void end_root_scope(Table* table, uint32_t previous_root) {
    table->root_page_num = previous_root;
}

static bool cursor_key_equals(Cursor* cursor, uint32_t key) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) return false;
    uint32_t found = 0u;
    return tinydb_leaf_page_key_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &found) &&
           found == key;
}

static bool raw_value_to_payload(const TableSchema* schema,
                                 const void* value,
                                 uint32_t stored_length,
                                 TinyDBLeafPageFormat format,
                                 TinyDBRecordPayload* payload) {
    if (schema == NULL || value == NULL || payload == NULL ||
        schema->row_size == 0u || schema->row_size > ROW_SIZE) {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        if (stored_length < schema->row_size) return false;
        payload->length = schema->row_size;
        memcpy(payload->bytes, value, payload->length);
        return true;
    }
    if (format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) return false;

    if (stored_length == schema->row_size) {
        payload->length = schema->row_size;
        memcpy(payload->bytes, value, payload->length);
        return true;
    }
    return tinydb_row_envelope_decode(schema,
                                      value,
                                      stored_length,
                                      payload);
}

static bool cursor_to_record(const TableSchema* schema,
                             Cursor* cursor,
                             TinyDBRecord* record) {
    if (schema == NULL || cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL || record == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages ||
        schema->row_size == 0u || schema->row_size > ROW_SIZE) {
        return false;
    }

    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) return false;

    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &stored_length) ||
        value == NULL) {
        return false;
    }

    TinyDBRecordPayload payload;
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    if (!raw_value_to_payload(schema,
                              value,
                              stored_length,
                              format,
                              &payload)) {
        return false;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_to_record(schema,
                                           &payload,
                                           record,
                                           message,
                                           sizeof(message));
}

bool tinydb_record_find(Table* table,
                        const TableSchema* schema,
                        uint32_t id,
                        TinyDBRecord* record) {
    if (table == NULL || schema == NULL || record == NULL) return false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, message, sizeof(message))) {
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    bool found = cursor_key_equals(cursor, id) &&
                 cursor_to_record(schema, cursor, record);
    free(cursor);
    end_root_scope(table, previous_root);
    return found;
}

uint32_t tinydb_record_scan(Table* table,
                            const TableSchema* schema,
                            TinyDBRecordVisitor visitor,
                            void* context) {
    if (table == NULL || schema == NULL) return 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, message, sizeof(message))) {
        return 0u;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_start(table);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        return 0u;
    }

    uint32_t count = 0u;
    bool corrupt = false;
    while (!cursor->end_of_table) {
        TinyDBRecord record;
        if (!cursor_to_record(schema, cursor, &record)) {
            corrupt = true;
            break;
        }
        count++;
        if (visitor != NULL && !visitor(schema, &record, context)) break;
        if (!tinydb_leaf_read_advance_checked(cursor)) {
            corrupt = true;
            break;
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);
    return corrupt ? 0u : count;
}
