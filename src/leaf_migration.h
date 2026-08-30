#ifndef LEAF_MIGRATION_H
#define LEAF_MIGRATION_H

#include "table.h"

#include <stddef.h>

/* These helpers migrate raw page images in memory only. They never touch the
 * Pager, WAL, dirty state, or checksum trailer. Callers provide distinct
 * source/destination PAGE_SIZE buffers; only PAGE_USABLE_SIZE bytes of a
 * successful destination are replaced.
 *
 * V1 -> V2 needs the schema's logical serialized row size so historical
 * ROW_SIZE padding is discarded instead of becoming variable-length payload.
 * The compact variant additionally wraps each logical value in a schema-aware
 * row envelope. That form is suitable for append-only schema evolution because
 * a later schema generation can validate the stored prefix fingerprint and
 * materialize newly appended fields as defaults.
 *
 * V2 -> V1 is deliberately conditional: every payload must fit ROW_SIZE and
 * the V2 slot count must fit the historical fixed-cell capacity. */
bool tinydb_leaf_migrate_v1_to_v2(const void* source,
                                   size_t source_capacity,
                                   uint32_t logical_value_length,
                                   void* destination,
                                   size_t destination_capacity);

bool tinydb_leaf_migrate_v1_to_compact_v2(const void* source,
                                           size_t source_capacity,
                                           const TableSchema* schema,
                                           void* destination,
                                           size_t destination_capacity);

bool tinydb_leaf_v2_can_downgrade_to_v1(const void* source,
                                        size_t source_capacity);

bool tinydb_leaf_migrate_v2_to_v1(const void* source,
                                   size_t source_capacity,
                                   void* destination,
                                   size_t destination_capacity);

#endif /* LEAF_MIGRATION_H */
