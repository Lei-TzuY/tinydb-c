#include "generic_index_epoch.h"
#include "leaf_mutation_policy.h"
#include "record.h"
#include "record_update_mixed.h"

#include <stdio.h>

bool tinydb_record_insert_base(Table* table,
                               const TableSchema* schema,
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
    if (schema == NULL) {
        set_message(message,
                    message_size,
                    "schema is required before generic mutation");
        return false;
    }
    return tinydb_leaf_tree_mutation_supported(table,
                                               schema->root_page_num,
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
    /* Existing-row UPDATE does not alter the B+ tree key set, separators, or
     * leaf sibling structure. It may therefore target either fixed V1 or
     * slotted V2 leaves while INSERT/DELETE stay behind the V1 structural
     * mutation gate until V2 split/rebalance recovery exists. */
    if (table == NULL || schema == NULL) {
        set_message(message,
                    message_size,
                    "table and schema are required before generic mutation");
        return false;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return tinydb_record_update_existing_mixed(table,
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
