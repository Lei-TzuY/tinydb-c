#include "generic_index_epoch.h"
#include "leaf_page_access.h"
#include "record.h"

#include <stdio.h>

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

static bool subtree_is_fixed_v1(Table* table,
                                uint32_t page_num,
                                uint32_t depth,
                                char* message,
                                size_t message_size) {
    if (page_num == INVALID_PAGE_NUM || depth > 64u) {
        set_message(message,
                    message_size,
                    "unable to validate generic B+ tree before mutation");
        return false;
    }

    void* node = get_page(table->pager, page_num);
    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        TinyDBLeafPageFormat format =
            tinydb_leaf_format_detect_page(node, PAGE_SIZE);
        if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) return true;
        if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
            set_message(message,
                        message_size,
                        "slotted leaf V2 pages are read-only until V2 mutation and split recovery are enabled");
            return false;
        }
        set_message(message,
                    message_size,
                    "unknown or corrupt leaf format blocks generic mutation");
        return false;
    }

    if (type != NODE_INTERNAL) {
        set_message(message,
                    message_size,
                    "unknown B+ tree node type blocks generic mutation");
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        set_message(message,
                    message_size,
                    "corrupt internal node blocks generic mutation");
        return false;
    }

    /* get_page() may evict and reuse the frame backing `node`. Snapshot every
     * child pointer before recursing so a deep tree walk never dereferences a
     * stale raw page pointer after buffer-pool pressure. */
    uint32_t child_pages[INTERNAL_NODE_MAX_KEYS + 1u];
    for (uint32_t i = 0u; i <= num_keys; i++) {
        child_pages[i] = *internal_node_child(node, i);
    }

    for (uint32_t i = 0u; i <= num_keys; i++) {
        if (!subtree_is_fixed_v1(table,
                                 child_pages[i],
                                 depth + 1u,
                                 message,
                                 message_size)) {
            return false;
        }
    }
    return true;
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
    return subtree_is_fixed_v1(table,
                               schema->root_page_num,
                               0u,
                               message,
                               message_size);
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
