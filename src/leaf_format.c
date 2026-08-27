#include "leaf_format.h"

static const TinyDBLeafFormatDescriptor CURRENT_FORMAT = {
    TINYDB_LEAF_FORMAT_FIXED_V1,
    false,
    PAGE_SIZE,
    PAGE_USABLE_SIZE,
    LEAF_NODE_HEADER_SIZE,
    LEAF_NODE_KEY_SIZE,
    LEAF_NODE_VALUE_SIZE,
    LEAF_NODE_CELL_SIZE,
    LEAF_NODE_MAX_CELLS
};

const TinyDBLeafFormatDescriptor* tinydb_leaf_format_current(void) {
    return &CURRENT_FORMAT;
}

bool tinydb_leaf_format_validate_current(void) {
    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    if (format->format_version != TINYDB_LEAF_FORMAT_FIXED_V1 ||
        format->variable_length_values ||
        format->page_size != PAGE_SIZE ||
        format->usable_size != PAGE_USABLE_SIZE ||
        format->header_size != LEAF_NODE_HEADER_SIZE ||
        format->key_size != LEAF_NODE_KEY_SIZE ||
        format->value_capacity != LEAF_NODE_VALUE_SIZE ||
        format->cell_size != LEAF_NODE_CELL_SIZE ||
        format->max_cells != LEAF_NODE_MAX_CELLS) {
        return false;
    }
    if (format->usable_size < format->header_size ||
        format->cell_size != format->key_size + format->value_capacity ||
        format->cell_size == 0u) {
        return false;
    }
    return format->max_cells ==
        (format->usable_size - format->header_size) / format->cell_size;
}

bool tinydb_leaf_format_can_store_value(uint32_t length) {
    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    return tinydb_leaf_format_validate_current() &&
           length > 0u &&
           length <= format->value_capacity;
}
