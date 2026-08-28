#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_tree_split_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tinydb_record_insert_mixed_base(Table* table,
                                     const TableSchema* schema,
                                     const TinyDBValue* values,
                                     uint32_t value_count,
                                     char* message,
                                     size_t message_size);

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

static uint32_t read_u32_native(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool peek_unused_page_num(const Pager* pager, uint32_t* page_num) {
    if (pager == NULL || page_num == NULL) return false;
    if (pager->free_page_count > 0u) {
        *page_num = pager->free_pages[pager->free_page_count - 1u];
        return *page_num != 0u && *page_num < pager->num_pages;
    }
    if (pager->num_pages == INVALID_PAGE_NUM) return false;
    *page_num = pager->num_pages;
    return true;
}

static bool encode_compact_insert(const TableSchema* schema,
                                  const TinyDBValue* values,
                                  uint32_t value_count,
                                  uint32_t* key,
                                  unsigned char envelope[PAGE_SIZE],
                                  uint32_t* envelope_length,
                                  char* message,
                                  size_t message_size) {
    if (schema == NULL || values == NULL || key == NULL || envelope == NULL ||
        envelope_length == NULL ||
        !tinydb_schema_supports_records(schema, message, message_size)) {
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
    if (payload.length < sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT requires an encoded primary key");
        return false;
    }
    memcpy(key, payload.bytes, sizeof(*key));

    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               envelope,
                                               PAGE_SIZE,
                                               envelope_length) ||
        *envelope_length == 0u || *envelope_length > UINT16_MAX) {
        set_message(message,
                    message_size,
                    "unable to encode compact V2 row for split INSERT");
        return false;
    }
    return true;
}

static bool previous_boundary_allows(Table* table,
                                     uint32_t previous_page_num,
                                     uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (previous_page_num >= table->pager->num_pages) return false;

    unsigned char previous_page[PAGE_SIZE];
    memcpy(previous_page,
           get_page(table->pager, previous_page_num),
           sizeof(previous_page));
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_page_count(previous_page, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(previous_page,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static bool publish_page(Pager* pager,
                         uint32_t page_num,
                         const unsigned char image[PAGE_SIZE]) {
    if (pager == NULL || image == NULL || page_num == INVALID_PAGE_NUM) {
        return false;
    }
    void* page = get_page(pager, page_num);
    if (page == NULL) return false;
    memcpy(page, image, PAGE_USABLE_SIZE);
    mark_page_dirty(pager, page_num);
    return true;
}

static bool split_insert_slotted_v2(Table* table,
                                    const TableSchema* schema,
                                    const TinyDBValue* values,
                                    uint32_t value_count,
                                    bool* applicable,
                                    char* message,
                                    size_t message_size) {
    if (applicable != NULL) *applicable = false;
    if (table == NULL || table->pager == NULL || schema == NULL) return false;

    uint32_t key = 0u;
    uint32_t envelope_length = 0u;
    unsigned char envelope[PAGE_SIZE];
    memset(envelope, 0, sizeof(envelope));
    if (!encode_compact_insert(schema,
                               values,
                               value_count,
                               &key,
                               envelope,
                               &envelope_length,
                               message,
                               message_size)) {
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    uint32_t left_page_num = cursor->page_num;
    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before,
           get_page(table->pager, left_page_num),
           sizeof(left_before));

    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (tinydb_leaf_format_detect_page(left_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(left_before, PAGE_SIZE) ||
        !tinydb_leaf_page_count(left_before, PAGE_SIZE, &count) || count < 2u) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    if (cursor->cell_num < count &&
        tinydb_leaf_page_key_at(left_before,
                                PAGE_SIZE,
                                cursor->cell_num,
                                &found_key) &&
        found_key == key) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "duplicate primary key");
        return false;
    }

    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE)) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }
    if (applicable != NULL) *applicable = true;

    if (left_before[IS_ROOT_OFFSET] != 0u) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 root-leaf split INSERT remains fail-closed");
        return false;
    }

    uint32_t old_left_max = 0u;
    if (!tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &old_left_max) ||
        key >= old_left_max) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT must preserve the existing child upper boundary");
        return false;
    }

    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    if (!tinydb_leaf_page_prev(left_before,
                               PAGE_SIZE,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(left_before, PAGE_SIZE, &next_page_num) ||
        next_page_num == 0u || next_page_num >= table->pager->num_pages ||
        next_page_num == left_page_num ||
        !previous_boundary_allows(table, previous_page_num, key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT requires a valid non-tail sibling range");
        return false;
    }

    uint32_t parent_page_num = read_u32_native(
        left_before + PARENT_POINTER_OFFSET);
    if (parent_page_num == 0u || parent_page_num >= table->pager->num_pages ||
        parent_page_num == left_page_num || parent_page_num == next_page_num) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT requires an existing parent page");
        return false;
    }

    free(cursor);
    end_root_scope(table, previous_root);

    unsigned char next_before[PAGE_SIZE];
    unsigned char parent_before[PAGE_SIZE];
    memcpy(next_before,
           get_page(table->pager, next_page_num),
           sizeof(next_before));
    memcpy(parent_before,
           get_page(table->pager, parent_page_num),
           sizeof(parent_before));
    if (tinydb_leaf_format_detect_page(next_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(next_before, PAGE_SIZE) ||
        get_node_type(parent_before) != NODE_INTERNAL) {
        set_message(message,
                    message_size,
                    "production V2 split currently requires a V2 next sibling and an existing internal parent");
        return false;
    }

    uint32_t right_page_num = 0u;
    if (!peek_unused_page_num(table->pager, &right_page_num) ||
        right_page_num == left_page_num || right_page_num == next_page_num ||
        right_page_num == parent_page_num) {
        set_message(message,
                    message_size,
                    "unable to reserve a distinct page number for slotted V2 split");
        return false;
    }

    unsigned char left_after[PAGE_SIZE];
    unsigned char right_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    unsigned char parent_after[PAGE_SIZE];
    memcpy(left_after, left_before, sizeof(left_after));
    memset(right_after, 0, sizeof(right_after));
    memcpy(next_after, next_before, sizeof(next_after));
    memcpy(parent_after, parent_before, sizeof(parent_after));

    if (!tinydb_slotted_leaf_v2_stage_tree_split_nonroot_with_next(
            left_after,
            PAGE_SIZE,
            left_page_num,
            right_after,
            PAGE_SIZE,
            right_page_num,
            next_after,
            PAGE_SIZE,
            next_page_num,
            parent_after,
            PAGE_SIZE,
            NULL,
            NULL)) {
        set_message(message,
                    message_size,
                    "slotted V2 split staging rejected parent overflow or inconsistent topology");
        return false;
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    uint32_t left_max = 0u;
    uint32_t right_max = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &right_max) ||
        right_max != old_left_max) {
        set_message(message,
                    message_size,
                    "slotted V2 split staging changed the existing child upper boundary");
        return false;
    }

    void* destination = key <= left_max ? (void*)left_after : (void*)right_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE)) {
        set_message(message,
                    message_size,
                    "byte-balanced V2 split did not leave enough space for the inserted row");
        return false;
    }

    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    if (!tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &checked_left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &checked_right_max) ||
        checked_left_max != left_max || checked_right_max != old_left_max) {
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT would invalidate staged parent separators");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    uint32_t claimed_page_num = get_unused_page_num(table->pager);
    if (claimed_page_num != right_page_num) {
        if (claimed_page_num < table->pager->num_pages &&
            claimed_page_num != 0u) {
            pager_free_page(table->pager, claimed_page_num);
        }
        set_message(message,
                    message_size,
                    "slotted V2 split page reservation changed before publication");
        return false;
    }

    if (!publish_page(table->pager, left_page_num, left_after) ||
        !publish_page(table->pager, right_page_num, right_after) ||
        !publish_page(table->pager, next_page_num, next_after) ||
        !publish_page(table->pager, parent_page_num, parent_after)) {
        set_message(message,
                    message_size,
                    "unable to publish staged slotted V2 split pages");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    char base_message[TINYDB_RECORD_MESSAGE_MAX];
    base_message[0] = '\0';
    if (tinydb_record_insert_mixed_base(table,
                                        schema,
                                        values,
                                        value_count,
                                        base_message,
                                        sizeof(base_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    bool applicable = false;
    char split_message[TINYDB_RECORD_MESSAGE_MAX];
    split_message[0] = '\0';
    if (split_insert_slotted_v2(table,
                                schema,
                                values,
                                value_count,
                                &applicable,
                                split_message,
                                sizeof(split_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (applicable && split_message[0] != '\0') {
        set_message(message, message_size, split_message);
    } else if (base_message[0] != '\0') {
        set_message(message, message_size, base_message);
    } else if (split_message[0] != '\0') {
        set_message(message, message_size, split_message);
    } else {
        set_message(message,
                    message_size,
                    "generic insert is not supported for this leaf layout");
    }
    return false;
}
