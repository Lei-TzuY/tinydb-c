#ifndef LEAF_FORMAT_H
#define LEAF_FORMAT_H

#include "table.h"

#define TINYDB_LEAF_FORMAT_FIXED_V1 1u

typedef struct {
    uint32_t format_version;
    bool variable_length_values;
    uint32_t page_size;
    uint32_t usable_size;
    uint32_t header_size;
    uint32_t key_size;
    uint32_t value_capacity;
    uint32_t cell_size;
    uint32_t max_cells;
} TinyDBLeafFormatDescriptor;

/* Current on-disk leaf format. V1 is intentionally a descriptor around the
 * historical fixed-cell layout; it does not change any page bytes. A future
 * slotted-page V2 can switch the descriptor and storage implementation behind
 * the leaf_value seam without changing schema-aware record encoding. */
const TinyDBLeafFormatDescriptor* tinydb_leaf_format_current(void);

bool tinydb_leaf_format_validate_current(void);
bool tinydb_leaf_format_can_store_value(uint32_t length);

#endif /* LEAF_FORMAT_H */
