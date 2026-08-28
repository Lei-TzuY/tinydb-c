#include "leaf_mutation_policy.h"

#include "leaf_cursor_read.h"
#include "leaf_page_access.h"

#include <stdio.h>
#include <stdlib.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

bool tinydb_leaf_tree_mutation_supported(Table* table,
                                         uint32_t root_page_num,
                                         char* message,
                                         size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || table->pager == NULL ||
        root_page_num == INVALID_PAGE_NUM) {
        set_message(message,
                    message_size,
                    "table and a valid root are required before leaf mutation");
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = root_page_num;
    Cursor* cursor = tinydb_leaf_read_start(table);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        set_message(message,
                    message_size,
                    "unable to locate the first leaf before mutation");
        return false;
    }

    uint32_t page_num = cursor->page_num;
    free(cursor);

    uint32_t traversed = 0u;
    const uint32_t traversal_limit = 1000000u;
    while (true) {
        if (page_num == INVALID_PAGE_NUM || traversed++ >= traversal_limit) {
            table->root_page_num = previous_root;
            set_message(message,
                        message_size,
                        "corrupt or cyclic leaf chain blocks mutation");
            return false;
        }

        void* page = get_page(table->pager, page_num);
        TinyDBLeafPageFormat format =
            tinydb_leaf_format_detect_page(page, PAGE_SIZE);
        if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
            table->root_page_num = previous_root;
            set_message(message,
                        message_size,
                        "slotted leaf V2 pages are read-only until V2 mutation and split recovery are enabled");
            return false;
        }
        if (format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
            table->root_page_num = previous_root;
            set_message(message,
                        message_size,
                        "unknown or corrupt leaf format blocks mutation");
            return false;
        }

        uint32_t next_page = 0u;
        if (!tinydb_leaf_page_next(page, PAGE_SIZE, &next_page)) {
            table->root_page_num = previous_root;
            set_message(message,
                        message_size,
                        "unable to validate the leaf chain before mutation");
            return false;
        }
        if (next_page == 0u) break;
        page_num = next_page;
    }

    table->root_page_num = previous_root;
    return true;
}
