#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "record_delete_v2_empty_leaf.h"
#include "record_delete_v2_root_leaf_collapse.h"
#include "slotted_leaf_v2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool tinydb_record_delete_v2_empty_base(Table* table,
                                        const TableSchema* schema,
                                        uint32_t id,
                                        char* message,
                                        size_t message_size);

static void begin_root_scope(Table* table,
                             const TableSchema* schema,
                             uint32_t* previous_root) {
    *previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
}

static void end_root_scope(Table* table, uint32_t previous_root) {
    table->root_page_num = previous_root;
}

bool tinydb_record_delete(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          char* message,
                          size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL) {
        return tinydb_record_delete_v2_empty_base(table,
                                                  schema,
                                                  id,
                                                  message,
                                                  message_size);
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    bool candidate = false;
    uint32_t leaf_page_num = INVALID_PAGE_NUM;
    unsigned char leaf_before[PAGE_SIZE];
    memset(leaf_before, 0, sizeof(leaf_before));

    if (cursor != NULL && cursor->page_num != 0u &&
        cursor->page_num != INVALID_PAGE_NUM &&
        cursor->page_num < table->pager->num_pages &&
        cursor->page_num != schema->root_page_num) {
        leaf_page_num = cursor->page_num;
        memcpy(leaf_before,
               get_page(table->pager, leaf_page_num),
               sizeof(leaf_before));
        if (tinydb_leaf_format_detect_page(leaf_before, PAGE_SIZE) ==
                TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
            tinydb_slotted_leaf_v2_validate(leaf_before, PAGE_SIZE)) {
            uint32_t count = 0u;
            uint32_t key = 0u;
            candidate = tinydb_leaf_page_count(leaf_before,
                                               PAGE_SIZE,
                                               &count) &&
                        count == 1u &&
                        cursor->cell_num == 0u &&
                        tinydb_leaf_page_key_at(leaf_before,
                                                PAGE_SIZE,
                                                0u,
                                                &key) &&
                        key == id;
        }
    }
    free(cursor);
    end_root_scope(table, previous_root);

    if (candidate) {
        TinyDBRootLeafCollapseResult collapse =
            tinydb_try_delete_v2_root_leaf_collapse(table,
                                                    schema,
                                                    leaf_page_num,
                                                    leaf_before,
                                                    id,
                                                    message,
                                                    message_size);
        if (collapse == TINYDB_ROOT_LEAF_COLLAPSE_SUCCESS) return true;
        if (collapse == TINYDB_ROOT_LEAF_COLLAPSE_FAILURE) return false;

        return tinydb_delete_v2_empty_leaf(table,
                                           schema,
                                           leaf_page_num,
                                           leaf_before,
                                           id,
                                           message,
                                           message_size);
    }

    return tinydb_record_delete_v2_empty_base(table,
                                              schema,
                                              id,
                                              message,
                                              message_size);
}
