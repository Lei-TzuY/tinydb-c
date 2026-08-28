#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "record_payload.h"
#include "row_envelope.h"

#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
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
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (schema == NULL || value == NULL || payload == NULL ||
        !tinydb_record_payload_schema_supported(schema,
                                                message,
                                                sizeof(message))) {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        /* A fixed V1 slot can only contain schemas that physically fit it.
         * Wide schemas therefore fail closed here rather than being truncated. */
        if (stored_length < schema->row_size ||
            schema->row_size > TINYDB_RECORD_PAYLOAD_MAX) {
            return false;
        }
        payload->length = schema->row_size;
        memcpy(payload->bytes, value, payload->length);
        return true;
    }
    if (format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) return false;

    /* V2 accepts both migration-era raw logical payloads and compact row
     * envelopes. Neither path is constrained by the historical ROW_SIZE. */
    if (stored_length == schema->row_size) {
        if (stored_length > sizeof(payload->bytes)) return false;
        payload->length = stored_length;
        memcpy(payload->bytes, value, payload->length);
        return true;
    }
    return tinydb_row_envelope_decode(schema,
                                      value,
                                      stored_length,
                                      payload);
}

static bool cursor_to_payload(const TableSchema* schema,
                              Cursor* cursor,
                              TinyDBRecordPayload* payload) {
    if (schema == NULL || cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL || payload == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
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

    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    return raw_value_to_payload(schema,
                                value,
                                stored_length,
                                format,
                                payload);
}

static bool cursor_to_record(const TableSchema* schema,
                             Cursor* cursor,
                             TinyDBRecord* record) {
    if (record == NULL) return false;

    TinyDBRecordPayload payload;
    if (!cursor_to_payload(schema, cursor, &payload)) return false;

    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_to_record(schema,
                                           &payload,
                                           record,
                                           message,
                                           sizeof(message));
}

bool tinydb_record_payload_find(Table* table,
                                const TableSchema* schema,
                                uint32_t id,
                                TinyDBRecordPayload* payload,
                                char* message,
                                size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL || payload == NULL) {
        set_message(message, message_size, "table, schema, and payload are required");
        return false;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    bool found = cursor_key_equals(cursor, id) &&
                 cursor_to_payload(schema, cursor, payload);
    free(cursor);
    end_root_scope(table, previous_root);
    if (!found) {
        set_message(message, message_size, "primary key not found or row payload is invalid");
    }
    return found;
}

uint32_t tinydb_record_payload_scan(Table* table,
                                    const TableSchema* schema,
                                    TinyDBRecordPayloadVisitor visitor,
                                    void* context,
                                    bool* scan_complete,
                                    char* message,
                                    size_t message_size) {
    if (scan_complete != NULL) *scan_complete = false;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required");
        return 0u;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return 0u;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_start(table);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        set_message(message, message_size, "unable to start leaf scan");
        return 0u;
    }

    uint32_t count = 0u;
    bool complete = true;
    while (!cursor->end_of_table) {
        TinyDBRecordPayload payload;
        if (!cursor_to_payload(schema, cursor, &payload)) {
            complete = false;
            set_message(message, message_size, "unable to decode logical row payload during scan");
            break;
        }
        count++;
        if (visitor != NULL && !visitor(schema, &payload, context)) break;
        if (!tinydb_leaf_read_advance_checked(cursor)) {
            complete = false;
            set_message(message, message_size, "leaf scan encountered corrupt traversal metadata");
            break;
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);
    if (scan_complete != NULL) *scan_complete = complete;
    return complete ? count : 0u;
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
