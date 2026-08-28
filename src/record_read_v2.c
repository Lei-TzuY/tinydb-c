#include "leaf_page_access.h"
#include "leaf_value.h"
#include "record.h"
#include "record_payload.h"

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
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    uint32_t found = 0u;
    return tinydb_leaf_page_key_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &found) &&
           found == key;
}

static bool cursor_to_record(const TableSchema* schema,
                             Cursor* cursor,
                             TinyDBRecord* record) {
    TinyDBRecordPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.length = schema->row_size;
    if (!tinydb_leaf_value_read(cursor,
                                payload.bytes,
                                sizeof(payload.bytes),
                                payload.length)) {
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
    Cursor* cursor = table_find(table, id);
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
    Cursor* cursor = table_start(table);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        return 0u;
    }

    uint32_t count = 0u;
    while (!cursor->end_of_table) {
        TinyDBRecord record;
        if (!cursor_to_record(schema, cursor, &record)) break;
        count++;
        if (visitor != NULL && !visitor(schema, &record, context)) break;
        cursor_advance(cursor);
    }

    free(cursor);
    end_root_scope(table, previous_root);
    return count;
}
