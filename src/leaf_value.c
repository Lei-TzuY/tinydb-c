#include "leaf_value.h"

#include <string.h>

static bool valid_args(Cursor* cursor,
                       const void* bytes,
                       uint32_t length) {
    return cursor != NULL && bytes != NULL && length > 0u && length <= ROW_SIZE;
}

bool tinydb_leaf_value_insert(Cursor* cursor,
                              uint32_t key,
                              const void* bytes,
                              uint32_t length) {
    if (!valid_args(cursor, bytes, length)) return false;

    /* The legacy leaf API accepts Row only because its serializer predates
     * schema-aware records. Row's serialized fields are contiguous across the
     * full ROW_SIZE payload, so it remains a lossless compatibility carrier. */
    Row carrier;
    memset(&carrier, 0, sizeof(carrier));
    memcpy(&carrier, bytes, length);
    leaf_node_insert(cursor, key, &carrier);
    return true;
}

bool tinydb_leaf_value_write(Cursor* cursor,
                             const void* bytes,
                             uint32_t length) {
    if (!valid_args(cursor, bytes, length)) return false;

    void* slot = cursor_value(cursor);
    memset(slot, 0, ROW_SIZE);
    memcpy(slot, bytes, length);
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    return true;
}

bool tinydb_leaf_value_read(Cursor* cursor,
                            void* output,
                            size_t output_capacity,
                            uint32_t length) {
    if (cursor == NULL || output == NULL || length == 0u || length > ROW_SIZE ||
        output_capacity < length) {
        return false;
    }

    memcpy(output, cursor_value(cursor), length);
    return true;
}
