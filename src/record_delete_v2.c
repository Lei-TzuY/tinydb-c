#include "generic_index_epoch.h"
#include "internal_max_decrease_cascade_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "slotted_leaf_v2.h"
#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char* pages;
    uint32_t* page_nums;
    uint32_t count;
    uint32_t capacity;
} TinyDBDeleteAncestorPath;

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

static void ancestor_path_free(TinyDBDeleteAncestorPath* path) {
    if (path == NULL) return;
    free(path->pages);
    free(path->page_nums);
    memset(path, 0, sizeof(*path));
}

static bool ancestor_path_reserve(TinyDBDeleteAncestorPath* path,
                                  uint32_t capacity) {
    if (path == NULL) return false;
    if (capacity <= path->capacity) return true;

    size_t page_bytes = (size_t)capacity * PAGE_SIZE;
    if (page_bytes / PAGE_SIZE != (size_t)capacity) return false;
    size_t num_bytes = (size_t)capacity * sizeof(uint32_t);
    if (num_bytes / sizeof(uint32_t) != (size_t)capacity) return false;

    unsigned char* pages = (unsigned char*)realloc(path->pages, page_bytes);
    if (pages == NULL) return false;
    path->pages = pages;
    uint32_t* page_nums = (uint32_t*)realloc(path->page_nums, num_bytes);
    if (page_nums == NULL) return false;
    path->page_nums = page_nums;
    path->capacity = capacity;
    return true;
}

static bool collect_ancestor_path(Table* table,
                                  const TableSchema* schema,
                                  uint32_t leaf_page_num,
                                  const unsigned char leaf_page[PAGE_SIZE],
                                  TinyDBDeleteAncestorPath* path) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        leaf_page == NULL || path == NULL ||
        leaf_page_num == schema->root_page_num) {
        return false;
    }
    memset(path, 0, sizeof(*path));

    uint32_t current_page_num = read_u32_native(
        leaf_page + PARENT_POINTER_OFFSET);
    if (current_page_num == INVALID_PAGE_NUM ||
        current_page_num >= table->pager->num_pages ||
        current_page_num == leaf_page_num ||
        (current_page_num == 0u && schema->root_page_num != 0u)) {
        return false;
    }

    while (true) {
        if (path->count == path->capacity) {
            uint32_t next_capacity = path->capacity == 0u
                ? 4u
                : path->capacity * 2u;
            if (next_capacity <= path->capacity ||
                !ancestor_path_reserve(path, next_capacity)) {
                ancestor_path_free(path);
                return false;
            }
        }
        for (uint32_t i = 0u; i < path->count; i++) {
            if (path->page_nums[i] == current_page_num) {
                ancestor_path_free(path);
                return false;
            }
        }

        unsigned char* destination =
            path->pages + (size_t)path->count * PAGE_SIZE;
        memcpy(destination,
               get_page(table->pager, current_page_num),
               PAGE_SIZE);
        if (!tinydb_parent_stage_validate(destination, PAGE_SIZE)) {
            ancestor_path_free(path);
            return false;
        }
        path->page_nums[path->count] = current_page_num;
        path->count++;

        bool is_root = destination[IS_ROOT_OFFSET] != 0u;
        if (current_page_num == schema->root_page_num) {
            if (!is_root ||
                read_u32_native(destination + PARENT_POINTER_OFFSET) != 0u) {
                ancestor_path_free(path);
                return false;
            }
            return true;
        }
        if (is_root) {
            ancestor_path_free(path);
            return false;
        }

        uint32_t next_page_num = read_u32_native(
            destination + PARENT_POINTER_OFFSET);
        if (next_page_num == INVALID_PAGE_NUM ||
            next_page_num >= table->pager->num_pages ||
            next_page_num == current_page_num ||
            (next_page_num == 0u && schema->root_page_num != 0u)) {
            ancestor_path_free(path);
            return false;
        }
        current_page_num = next_page_num;
    }
}

static bool publish_delete_images(Table* table,
                                  uint32_t leaf_page_num,
                                  const unsigned char leaf_after[PAGE_SIZE],
                                  bool ancestor_changed,
                                  uint32_t ancestor_page_num,
                                  const unsigned char ancestor_after[PAGE_SIZE]) {
    if (table == NULL || table->pager == NULL || leaf_after == NULL) return false;

    unsigned char* ancestor_target = NULL;
    if (ancestor_changed) {
        ancestor_target =
            (unsigned char*)get_page(table->pager, ancestor_page_num);
        if (ancestor_target == NULL || ancestor_after == NULL) return false;
    }
    unsigned char* leaf_target =
        (unsigned char*)get_page(table->pager, leaf_page_num);
    if (leaf_target == NULL) return false;

    TinyDBV2PublishEntry entries[2];
    uint32_t count = 1u;
    entries[0].page_num = leaf_page_num;
    entries[0].target = leaf_target;
    entries[0].staged = leaf_after;
    if (ancestor_changed) {
        entries[1].page_num = ancestor_page_num;
        entries[1].target = ancestor_target;
        entries[1].staged = ancestor_after;
        count = 2u;
    }

    if (!tinydb_v2_publish_batch(entries,
                                 count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        return false;
    }
    mark_page_dirty(table->pager, leaf_page_num);
    if (ancestor_changed) mark_page_dirty(table->pager, ancestor_page_num);
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
    TinyDBDeleteAncestorPath ancestors;
    memset(&ancestors, 0, sizeof(ancestors));
    bool ancestor_changed = false;
    uint32_t changed_level = UINT32_MAX;
    uint32_t ancestor_page_num = INVALID_PAGE_NUM;
    const unsigned char* ancestor_after = NULL;

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
                                     &new_leaf_max) ||
            !collect_ancestor_path(table,
                                   schema,
                                   leaf_page_num,
                                   leaf_before,
                                   &ancestors) ||
            !tinydb_stage_internal_max_decrease_cascade(
                ancestors.pages,
                PAGE_SIZE,
                ancestors.page_nums,
                ancestors.count,
                leaf_page_num,
                id,
                new_leaf_max,
                &changed_level,
                &ancestor_changed)) {
            ancestor_path_free(&ancestors);
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 delete could not propagate the lowered subtree maximum");
            return false;
        }
        if (changed_level == UINT32_MAX || changed_level >= ancestors.count) {
            ancestor_path_free(&ancestors);
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 max-key propagation stopped outside the ancestor chain");
            return false;
        }
        if (ancestor_changed) {
            ancestor_page_num = ancestors.page_nums[changed_level];
            ancestor_after = ancestors.pages +
                (size_t)changed_level * PAGE_SIZE;
        }
    }

    unsigned char leaf_after[PAGE_SIZE];
    memcpy(leaf_after, leaf_before, sizeof(leaf_after));
    if (!tinydb_slotted_leaf_v2_delete(leaf_after, PAGE_SIZE, id) ||
        !tinydb_slotted_leaf_v2_validate(leaf_after, PAGE_SIZE)) {
        ancestor_path_free(&ancestors);
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
            ancestor_path_free(&ancestors);
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 max-key delete produced an invalid child boundary");
            return false;
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        ancestor_path_free(&ancestors);
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    if (!publish_delete_images(table,
                               leaf_page_num,
                               leaf_after,
                               ancestor_changed,
                               ancestor_page_num,
                               ancestor_after)) {
        ancestor_path_free(&ancestors);
        set_message(message,
                    message_size,
                    "unable to atomically publish the slotted V2 delete");
        return false;
    }
    ancestor_path_free(&ancestors);

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
