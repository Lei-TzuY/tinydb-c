#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record_payload.h"
#include "record_payload_root_split.h"
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

static bool payload_insert_preserves_topology(Table* table,
                                              const TableSchema* schema,
                                              uint32_t page_num,
                                              const void* page,
                                              uint32_t key) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        page == NULL || page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t count = 0u;
    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
        !tinydb_leaf_page_prev(page, PAGE_SIZE, &previous_page_num) ||
        !tinydb_leaf_page_next(page, PAGE_SIZE, &next_page_num)) {
        return false;
    }

    if (page_num == schema->root_page_num) {
        /* A root leaf owns the whole key space and has no parent separator. */
        return previous_page_num == 0u && next_page_num == 0u;
    }

    if (count == 0u) return false;

    uint32_t last_key = 0u;
    if (!tinydb_leaf_page_key_at(page,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &last_key) ||
        key >= last_key) {
        /* A non-root child maximum is routing metadata. Separator propagation
         * for payload-native non-root inserts is intentionally a later seam. */
        return false;
    }

    if (previous_page_num == 0u) return true;
    if (previous_page_num == page_num ||
        previous_page_num >= table->pager->num_pages) {
        return false;
    }

    void* previous_page = get_page(table->pager, previous_page_num);
    uint32_t previous_next = 0u;
    uint32_t previous_count = 0u;
    uint32_t previous_last_key = 0u;
    if (tinydb_leaf_format_detect_page(previous_page, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(previous_page, PAGE_SIZE) ||
        !tinydb_leaf_page_next(previous_page, PAGE_SIZE, &previous_next) ||
        previous_next != page_num ||
        !tinydb_leaf_page_count(previous_page, PAGE_SIZE, &previous_count) ||
        previous_count == 0u ||
        !tinydb_leaf_page_key_at(previous_page,
                                 PAGE_SIZE,
                                 previous_count - 1u,
                                 &previous_last_key)) {
        return false;
    }

    return key > previous_last_key;
}

bool tinydb_record_payload_insert(Table* table,
                                  const TableSchema* schema,
                                  const TinyDBRecordPayload* payload,
                                  char* message,
                                  size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || table->pager == NULL || schema == NULL ||
        payload == NULL) {
        set_message(message,
                    message_size,
                    "table, schema, and payload are required for payload-native INSERT");
        return false;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (schema->num_columns == 0u ||
        schema->columns[0].type != COL_TYPE_INT ||
        !ci_equal(schema->columns[0].name, "id")) {
        set_message(message,
                    message_size,
                    "payload-native INSERT requires the first column to be id INT");
        return false;
    }
    if (ci_equal(schema->name, "users")) {
        set_message(message,
                    message_size,
                    "payload-native INSERT excludes the legacy users table so its secondary indexes stay synchronized");
        return false;
    }
    if (schema->root_page_num >= table->pager->num_pages ||
        payload->length != schema->row_size ||
        payload->length < sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "payload length or schema root is invalid for payload-native INSERT");
        return false;
    }

    uint32_t key = 0u;
    memcpy(&key, payload->bytes, sizeof(key));

    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               payload,
                                               envelope,
                                               sizeof(envelope),
                                               &envelope_length) ||
        envelope_length == 0u || envelope_length > UINT16_MAX) {
        set_message(message,
                    message_size,
                    "unable to encode compact V2 envelope for payload-native INSERT");
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "unable to locate target leaf for payload-native INSERT");
        return false;
    }
    if (cursor_matches_key(cursor, key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "duplicate primary key");
        return false;
    }

    uint32_t target_page_num = cursor->page_num;
    void* page = get_page(table->pager, target_page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) ||
        !payload_insert_preserves_topology(table,
                                           schema,
                                           target_page_num,
                                           page,
                                           key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "payload-native INSERT is limited to a topology-neutral compact V2 leaf and cannot change parent separators or sibling key boundaries");
        return false;
    }

    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (required > tinydb_slotted_leaf_v2_free_bytes(page, PAGE_SIZE)) {
        bool root_leaf = target_page_num == schema->root_page_num;
        free(cursor);
        end_root_scope(table, previous_root);

        if (root_leaf) {
            bool applicable = false;
            if (tinydb_record_payload_try_root_split(table,
                                                     schema,
                                                     key,
                                                     envelope,
                                                     envelope_length,
                                                     &applicable,
                                                     message,
                                                     message_size)) {
                return true;
            }
            if (applicable) return false;
        }

        set_message(message,
                    message_size,
                    "payload-native INSERT requires existing non-root V2 leaf space; non-root split propagation is not implemented yet");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    unsigned char before[PAGE_USABLE_SIZE];
    memcpy(before, page, sizeof(before));
    bool inserted = tinydb_slotted_leaf_v2_insert(page,
                                                  PAGE_SIZE,
                                                  key,
                                                  envelope,
                                                  (uint16_t)envelope_length);
    if (!inserted) {
        memcpy(page, before, sizeof(before));
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "payload-native INSERT failed without publishing the row");
        return false;
    }

    mark_page_dirty(table->pager, target_page_num);
    free(cursor);
    end_root_scope(table, previous_root);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
