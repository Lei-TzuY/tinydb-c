#ifndef RECORD_UPDATE_MIXED_H
#define RECORD_UPDATE_MIXED_H

#include "record.h"

#include <stddef.h>

/* Update an existing schema-aware row without changing the B+ tree key set.
 * Fixed V1 leaves retain their canonical fixed-slot representation. Slotted V2
 * leaves are rewritten with the compact versioned row envelope. Structural
 * insert/delete/split mutation remains guarded elsewhere. */
bool tinydb_record_update_existing_mixed(
    Table* table,
    const TableSchema* schema,
    uint32_t id,
    const TinyDBValue* values,
    uint32_t value_count,
    char* message,
    size_t message_size);

#endif /* RECORD_UPDATE_MIXED_H */
