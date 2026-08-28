#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_page_access.h"
#include "record.h"

#include <stdio.h>
#include <stdlib.h>

bool tinydb_record_insert_base(Table* table,
                               const TableSchema* schema,
                               const TinyDBValue* values,
                               uint32_t value_count,
                               char* message,
                               size_t message_size);

bool tinydb_record_update_base(Table* table,
                               const TableSchema* schema,
                               uint32_t id,
                               const TinyDBValue* values,
                               uint32_t value_count,
                               char* message,
                               size_t message_size);

bool tinydb_record_delete_base(Table* table,
                               const TableSchema* schema,
                               uint32_t id,
                               char* message,
                               size_t message_size);

uint32_t tinydb_record_delete_all_base(Table* table,
                                       const TableSchema* schema,
                                       char* message,
                                       size_t message_size);

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static bool mutation_tree_is_fixed_v1(Table* table,
                                      const TableSchema* schema,
                                      char* message,
                                      size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL) {
        set_message(message,
                    message_size,
                    "table and schema are required before generic mutation");
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_start(table);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        set_message(message,
                    message_size,
                    "unable to locate the first leaf before generic mutation");
        return false;
    }

    uint32_t page_num = cursor->page_num;
    free(cursor);

    /* A sibling cycle is corruption. Keep the guard independent of the main
     * file page count because committed WAL shadow pages can legitimately sit
     * beyond the checkpointed file length until recovery/checkpoint catches up. */
    uint32_t traversed = 0u;
    const uint32_t traversal_limit = 1000000u;
    while (true) {
        if (page_num == INVALID_PAGE_NUM || traversed++ >= traversal_limit) {
            table->root_page_num = previous_root;
            set_message(message,
                        message_size,
                        "corrupt or cyclic leaf chain blocks generic mutation");
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
                        "unknown or corrupt leaf format blocks generic mutation");
            return false;
        }

        uint32_t next_page = 0u;
        if (!tinydb_leaf_page_next(page, PAGE_SIZE, &next_page)) {
            table->root_page_num = previous_root;
            set_message(message,
                        message_size,
                        "unable to validate the leaf chain before generic mutation");
            return false;
        }
        if (next_page == 0u) break;
        page_num = next_page;
    }

    table->root_page_num = previous_root;
    return true;
}

static bool prepare_index_epoch(Table* table,
                                const TableSchema* schema,
                                char* message,
                                size_t message_size) {
    if (tinydb_generic_index_epoch_before_mutation(table, schema)) return true;
    set_message(message,
                message_size,
                "unable to persist generic-index mutation epoch");
    return false;
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    if (!mutation_tree_is_fixed_v1(table, schema, message, message_size)) {
        return false;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return tinydb_record_insert_base(table,
                                     schema,
                                     values,
                                     value_count,
                                     message,
                                     message_size);
}

bool tinydb_record_update(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    if (!mutation_tree_is_fixed_v1(table, schema, message, message_size)) {
        return false;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return tinydb_record_update_base(table,
                                     schema,
                                     id,
                                     values,
                                     value_count,
                                     message,
                                     message_size);
}

bool tinydb_record_delete(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          char* message,
                          size_t message_size) {
    if (!mutation_tree_is_fixed_v1(table, schema, message, message_size)) {
        return false;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return tinydb_record_delete_base(table, schema, id, message, message_size);
}

uint32_t tinydb_record_delete_all(Table* table,
                                  const TableSchema* schema,
                                  char* message,
                                  size_t message_size) {
    if (!mutation_tree_is_fixed_v1(table, schema, message, message_size)) {
        return 0u;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return 0u;
    return tinydb_record_delete_all_base(table, schema, message, message_size);
}
