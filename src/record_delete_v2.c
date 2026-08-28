#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "slotted_leaf_v2.h"
#include "slotted_v2_parent_max_stage.h"
#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tinydb_record_delete_mixed_base(Table* table,
                                     const TableSchema* schema,
                                     uint32_t id,
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

static bool publish_delete_images(Table* table,
                                  uint32_t leaf_page_num,
                                  const unsigned char leaf_after[PAGE_SIZE],
                                  bool parent_changed,
                                  uint32_t parent_page_num,
                                  const unsigned char parent_after[PAGE_SIZE]) {
    if (table == NULL || table->pager == NULL || leaf_after == NULL) return false;

    unsigned char* parent_target = NULL;
    if (parent_changed) {
        parent_target = (unsigned char*)get_page(table->pager, parent_page_num);
        if (parent_target == NULL) return false;
    }
    unsigned char* leaf_target =
        (unsigned char*)get_page(table->pager, leaf_page_num);
    if (leaf_target == NULL) return false;

    TinyDBV2PublishEntry entries[2];
    uint32_t count = 1u;
    entries[0].page_num = leaf_page_num;
    entries[0].target = leaf_target;
    entries[0].staged = leaf_after;
    if (parent_changed) {
        entries[1].page_num = parent_page_num;
        entries[1].target = parent_target;
        entries[1].staged = parent_after;
        count = 2u;
    }

    if (!tinydb_v2_publish_batch(entries,
                                 count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        return false;
    }
    mark_page_dirty(table->pager, leaf_page_num);
    if (parent_changed) mark_page_dirty(table->pager, parent_page_num);
    return true;
}

static bool delete_slotted_v2(Table* table,
                              const TableSchema* schema,
                              uint32_t id,
                              char* message,
                              size_t message_size) {
    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);

    Cursor* cursor = tinydb_leaf_read_find(table, id);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found");
        return false;
    }

    uint32_t leaf_page_num = cursor->page_num;
    unsigned char leaf_before[PAGE_SIZE];
    memcpy(leaf_before,
           get_page(table->pager, leaf_page_num),
           sizeof(leaf_before));
    if (tinydb_leaf_format_detect_page(leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(leaf_before, PAGE_SIZE)) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (!tinydb_leaf_page_count(leaf_before, PAGE_SIZE, &count) ||
        cursor->cell_num >= count ||
        !tinydb_leaf_page_key_at(leaf_before,
                                 PAGE_SIZE,
                                 cursor->cell_num,
                                 &found_key) ||
        found_key != id) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found");
        return false;
    }

    bool root_leaf = leaf_page_num == schema->root_page_num;
    bool deleting_max = cursor->cell_num + 1u == count;
    unsigned char parent_after[PAGE_SIZE];
    memset(parent_after, 0, sizeof(parent_after));
    uint32_t parent_page_num = INVALID_PAGE_NUM;
    bool parent_changed = false;

    if (!root_leaf && deleting_max) {
        if (count < 2u) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 delete would empty a non-root leaf and remains fail-closed");
            return false;
        }

        uint32_t new_leaf_max = 0u;
        if (!tinydb_leaf_page_key_at(leaf_before,
                                     PAGE_SIZE,
                                     count - 2u,
                                     &new_leaf_max)) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 delete could not determine the replacement child maximum");
            return false;
        }

        parent_page_num = read_u32_native(
            leaf_before + PARENT_POINTER_OFFSET);
        if (parent_page_num == 0u ||
            parent_page_num >= table->pager->num_pages ||
            parent_page_num == leaf_page_num) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 max-key delete requires a valid internal parent");
            return false;
        }

        unsigned char parent_before[PAGE_SIZE];
        memcpy(parent_before,
               get_page(table->pager, parent_page_num),
               sizeof(parent_before));
        memcpy(parent_after, parent_before, sizeof(parent_after));
        uint32_t child_index = UINT32_MAX;
        if (!tinydb_stage_parent_child_max_decrease(parent_after,
                                                     PAGE_SIZE,
                                                     leaf_page_num,
                                                     id,
                                                     new_leaf_max,
                                                     &child_index,
                                                     &parent_changed)) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 delete could not stage the parent separator decrease");
            return false;
        }

        uint32_t parent_keys = tinydb_parent_stage_read_u32(
            parent_before + INTERNAL_NODE_NUM_KEYS_OFFSET);
        if (child_index == parent_keys &&
            parent_page_num != schema->root_page_num) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 rightmost max delete requires recursive ancestor propagation and remains fail-closed");
            return false;
        }
    }

    unsigned char leaf_after[PAGE_SIZE];
    memcpy(leaf_after, leaf_before, sizeof(leaf_after));
    if (!tinydb_slotted_leaf_v2_delete(leaf_after, PAGE_SIZE, id) ||
        !tinydb_slotted_leaf_v2_validate(leaf_after, PAGE_SIZE)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 delete failed without modifying the page");
        return false;
    }

    if (!root_leaf && deleting_max) {
        uint32_t new_count = 0u;
        uint32_t checked_max = 0u;
        if (!tinydb_leaf_page_count(leaf_after, PAGE_SIZE, &new_count) ||
            new_count == 0u ||
            !tinydb_leaf_page_key_at(leaf_after,
                                     PAGE_SIZE,
                                     new_count - 1u,
                                     &checked_max) ||
            checked_max >= id) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 max-key delete produced an invalid child boundary");
            return false;
        }
        if (parent_changed) {
            uint32_t child_index = UINT32_MAX;
            bool changed_again = false;
            unsigned char verification[PAGE_SIZE];
            memcpy(verification, parent_after, sizeof(verification));
            if (tinydb_stage_parent_child_max_decrease(verification,
                                                        PAGE_SIZE,
                                                        leaf_page_num,
                                                        id,
                                                        checked_max,
                                                        &child_index,
                                                        &changed_again)) {
                free(cursor);
                end_root_scope(table, previous_root);
                set_message(message,
                            message_size,
                            "slotted V2 parent separator was not published exactly once");
                return false;
            }
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    if (!publish_delete_images(table,
                               leaf_page_num,
                               leaf_after,
                               parent_changed,
                               parent_page_num,
                               parent_after)) {
        set_message(message,
                    message_size,
                    "unable to atomically publish the slotted V2 delete");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_delete(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          char* message,
                          size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL) {
        set_message(message,
                    message_size,
                    "table and schema are required before generic mutation");
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    TinyDBLeafPageFormat format = TINYDB_LEAF_PAGE_FORMAT_UNKNOWN;
    if (cursor != NULL && cursor->page_num != INVALID_PAGE_NUM &&
        cursor->page_num < table->pager->num_pages) {
        void* page = get_page(table->pager, cursor->page_num);
        format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    }
    free(cursor);
    end_root_scope(table, previous_root);

    if (format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return tinydb_record_delete_mixed_base(table,
                                               schema,
                                               id,
                                               message,
                                               message_size);
    }

    return delete_slotted_v2(table,
                             schema,
                             id,
                             message,
                             message_size);
}
