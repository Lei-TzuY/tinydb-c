#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "slotted_leaf_v2.h"

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

static bool delete_topology_neutral_v2(Table* table,
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

    void* page = get_page(table->pager, cursor->page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
        cursor->cell_num >= count ||
        !tinydb_leaf_page_key_at(page,
                                 PAGE_SIZE,
                                 cursor->cell_num,
                                 &found_key) ||
        found_key != id) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found");
        return false;
    }

    /* Parent separators store the maximum key of the child to their left.
     * Removing any non-maximum key from a non-root V2 leaf is therefore
     * topology-neutral: no parent separator, sibling link, child pointer, or
     * page allocation changes. A root leaf has no parent separator and may
     * safely delete any key, including its final row. Cases that would change
     * a non-root maximum remain fail-closed until V2 rebalance/parent-update
     * recovery is connected to the production mutation path. */
    bool root_leaf = cursor->page_num == schema->root_page_num;
    if (!root_leaf && cursor->cell_num + 1u == count) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 delete would change a parent separator and remains fail-closed");
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

    unsigned char before[PAGE_SIZE];
    memcpy(before, page, sizeof(before));
    if (!tinydb_slotted_leaf_v2_delete(page, PAGE_SIZE, id)) {
        memcpy(page, before, sizeof(before));
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 delete failed without modifying the page");
        return false;
    }

    mark_page_dirty(table->pager, cursor->page_num);
    free(cursor);
    end_root_scope(table, previous_root);

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

    return delete_topology_neutral_v2(table,
                                      schema,
                                      id,
                                      message,
                                      message_size);
}
