#ifndef LEAF_CURSOR_READ_H
#define LEAF_CURSOR_READ_H

#include "table.h"

/* Read-only cursor API that understands both fixed V1 and slotted V2 leaves.
 * The historical table/cursor API remains the production V1 mutation path. */
Cursor* tinydb_leaf_read_find(Table* table, uint32_t key);
Cursor* tinydb_leaf_read_start(Table* table);
Cursor* tinydb_leaf_read_end(Table* table);
void* tinydb_leaf_read_value(Cursor* cursor);
void tinydb_leaf_read_advance(Cursor* cursor);
void tinydb_leaf_read_retreat(Cursor* cursor);

#endif /* LEAF_CURSOR_READ_H */
