#include "generic_index_epoch.h"
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

static bool prepare_index_epoch(Table* table,
                                const TableSchema* schema,
                                char* message,
                                size_t message_size) {
    if (tinydb_generic_index_epoch_before_mutation(table, schema)) return true;
    if (message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "%s",
                 "unable to persist generic-index mutation epoch");
    }
    return false;
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
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
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return tinydb_record_delete_base(table, schema, id, message, message_size);
}

uint32_t tinydb_record_delete_all(Table* table,
                                  const TableSchema* schema,
                                  char* message,
                                  size_t message_size) {
    if (!prepare_index_epoch(table, schema, message, message_size)) return 0;
    return tinydb_record_delete_all_base(table, schema, message, message_size);
}
