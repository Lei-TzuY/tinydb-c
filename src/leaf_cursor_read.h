#ifndef LEAF_CURSOR_READ_H
#define LEAF_CURSOR_READ_H

#include "leaf_migration.h"
#include "leaf_page_access.h"
#include "table.h"

#include <stdint.h>
#include <stdlib.h>

/* Read-only cursor API that understands both fixed V1 and slotted V2 leaves.
 * The historical table/cursor API remains the production V1 mutation path. */
Cursor* tinydb_leaf_read_find(Table* table, uint32_t key);
Cursor* tinydb_leaf_read_start(Table* table);
Cursor* tinydb_leaf_read_end(Table* table);
void* tinydb_leaf_read_value(Cursor* cursor);

/* Checked traversal distinguishes a legitimate end-of-chain transition from
 * corrupt/out-of-range sibling metadata. This lets schema-aware scans fail
 * closed instead of silently returning a truncated prefix. */
bool tinydb_leaf_read_advance_checked(Cursor* cursor);
bool tinydb_leaf_read_retreat_checked(Cursor* cursor);

/* Compatibility wrappers keep the existing read-cursor surface available to
 * probes/callers that do not need an explicit corruption status. */
void tinydb_leaf_read_advance(Cursor* cursor);
void tinydb_leaf_read_retreat(Cursor* cursor);

/* Whole-tree migration input seam. The visitor receives rows in strict B+tree
 * key order, already converted from canonical fixed-V1 payloads into canonical
 * compact-V2 historical-generation envelopes. A NULL visitor performs a pure
 * validation/counting pass.
 *
 * This routine intentionally does not publish or mutate a destination tree.
 * A staging-tree builder may perform writes in the visitor, but must discard
 * that unpublished staging tree if this scan returns false. Source traversal
 * fails closed on mixed-format leaves, invalid fixed rows, broken sibling
 * reciprocity/order, callback rejection, or other cursor corruption.
 *
 * rows_scanned is published only after the complete source tree succeeds, so
 * callers can distinguish an atomic successful scan from a rejected prefix. */
typedef bool (*TinyDBFixedV1CompactVisitor)(uint32_t key,
                                            const void* envelope,
                                            uint32_t envelope_length,
                                            void* context);

static inline bool tinydb_fixed_v1_tree_scan_compact_v2(
    Table* table,
    const TableSchema* schema,
    TinyDBFixedV1CompactVisitor visitor,
    void* context,
    uint64_t* rows_scanned) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;

    Cursor* cursor = tinydb_leaf_read_start(table);
    uint64_t count = 0u;
    bool ok = false;
    unsigned char envelope[PAGE_SIZE];

    if (cursor == NULL) goto done;
    while (!cursor->end_of_table) {
        if (cursor->page_num >= table->pager->num_pages) goto done;

        void* page = get_page(table->pager, cursor->page_num);
        if (get_node_type(page) != NODE_LEAF ||
            !tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)) {
            goto done;
        }

        uint32_t key = 0u;
        const void* value = NULL;
        uint32_t value_length = 0u;
        uint32_t envelope_length = 0u;
        if (!tinydb_leaf_page_key_at(page,
                                     PAGE_SIZE,
                                     cursor->cell_num,
                                     &key) ||
            !tinydb_leaf_page_value_at(page,
                                       PAGE_SIZE,
                                       cursor->cell_num,
                                       &value,
                                       &value_length) ||
            value_length != ROW_SIZE ||
            !tinydb_fixed_v1_row_encode_compact_v2(schema,
                                                    key,
                                                    value,
                                                    value_length,
                                                    envelope,
                                                    sizeof(envelope),
                                                    &envelope_length) ||
            envelope_length == 0u ||
            (visitor != NULL &&
             !visitor(key, envelope, envelope_length, context))) {
            goto done;
        }

        count++;
        if (!tinydb_leaf_read_advance_checked(cursor)) goto done;
    }

    ok = true;

done:
    free(cursor);
    table->root_page_num = previous_root;
    if (ok && rows_scanned != NULL) *rows_scanned = count;
    return ok;
}

#endif /* LEAF_CURSOR_READ_H */
