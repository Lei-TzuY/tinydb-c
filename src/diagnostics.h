#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "table.h"
#include "pager_try_pin.h"
#include "leaf_page_access.h"

#include <ctype.h>

#define TINYDB_DIAGNOSTIC_MESSAGE_MAX 256

typedef struct {
    uint32_t root_page_num;
    uint32_t total_rows;
    uint32_t leaf_pages;
    uint32_t internal_pages;
    uint32_t height;
} TinyDBTreeStats;

typedef struct {
    uint32_t table_count;
    uint32_t total_rows;
    uint32_t leaf_pages;
    uint32_t internal_pages;
    uint32_t max_height;
} TinyDBDatabaseTreeStats;

typedef struct {
    TinyDBDatabaseTreeStats aggregate;
    TinyDBTreeStats table_stats[MAX_TABLES];
} TinyDBCatalogTreeStatsSnapshot;

typedef struct {
    uint32_t total_pages;
    uint32_t owned_pages;
    uint32_t free_pages;
    uint32_t orphan_pages;
    uint32_t shared_pages;
} TinyDBPageOwnershipStats;

typedef struct {
    uint32_t table_count;
    TinyDBTreeStats table_stats[MAX_TABLES];
    TinyDBPageOwnershipStats ownership;
} TinyDBCatalogTreeCheck;

const TableSchema* tinydb_find_table_schema(const Table* table, const char* table_name);

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats);

/* ABI-additive variant that preserves the detailed non-fatal reason from the
 * tree walker (for example, buffer-pool backpressure or malformed leaf data).
 * The historical bool-only helper above remains available and delegates here. */
bool tinydb_get_tree_stats_diagnostic(Table* table,
                                      const char* table_name,
                                      TinyDBTreeStats* stats,
                                      char* message,
                                      size_t message_size);

/* Source-level catalog snapshot used by diagnostics-aware callers that need
 * aggregate and per-table statistics from the same traversal. Accumulation
 * stays local until every root succeeds, so BUSY/corruption/overflow cannot
 * publish a prefix of per-table results or plausible-looking aggregate totals. */
static inline bool tinydb_get_catalog_tree_stats_snapshot(
    Table* table,
    TinyDBCatalogTreeStatsSnapshot* snapshot,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (table == NULL || snapshot == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid catalog tree-stat arguments");
        }
        return false;
    }

    TinyDBCatalogTreeStatsSnapshot collected;
    memset(&collected, 0, sizeof(collected));
    collected.aggregate.table_count = table->catalog.num_tables;

    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        const char* table_name = table->catalog.schemas[i].name;
        TinyDBTreeStats* tree_stats = &collected.table_stats[i];
        char tree_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
        if (!tinydb_get_tree_stats_diagnostic(table,
                                               table_name,
                                               tree_stats,
                                               tree_message,
                                               sizeof(tree_message))) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "table '%s': %s",
                         table_name,
                         tree_message[0] != '\0'
                             ? tree_message
                             : "tree statistics unavailable");
            }
            return false;
        }
        if (UINT32_MAX - collected.aggregate.total_rows < tree_stats->total_rows ||
            UINT32_MAX - collected.aggregate.leaf_pages < tree_stats->leaf_pages ||
            UINT32_MAX - collected.aggregate.internal_pages < tree_stats->internal_pages) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "catalog tree statistics overflow at table '%s'",
                         table_name);
            }
            return false;
        }
        collected.aggregate.total_rows += tree_stats->total_rows;
        collected.aggregate.leaf_pages += tree_stats->leaf_pages;
        collected.aggregate.internal_pages += tree_stats->internal_pages;
        if (tree_stats->height > collected.aggregate.max_height) {
            collected.aggregate.max_height = tree_stats->height;
        }
    }

    *snapshot = collected;
    if (message != NULL && message_size > 0u) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u rows=%u leaf_pages=%u internal_pages=%u max_height=%u",
                 collected.aggregate.table_count,
                 collected.aggregate.total_rows,
                 collected.aggregate.leaf_pages,
                 collected.aggregate.internal_pages,
                 collected.aggregate.max_height);
    }
    return true;
}

/* Source-level catalog aggregate for diagnostics-aware callers. Preserve the
 * historical aggregate-only API while delegating traversal/publication rules
 * to the shared snapshot seam above. */
static inline bool tinydb_get_database_tree_stats(
    Table* table,
    TinyDBDatabaseTreeStats* stats,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (stats != NULL) memset(stats, 0, sizeof(*stats));
    if (table == NULL || stats == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid database tree-stat arguments");
        }
        return false;
    }

    TinyDBCatalogTreeStatsSnapshot snapshot;
    if (!tinydb_get_catalog_tree_stats_snapshot(table,
                                                &snapshot,
                                                message,
                                                message_size)) {
        return false;
    }
    *stats = snapshot.aggregate;
    return true;
}

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size);

bool tinydb_check_page_ownership(Table* table,
                                 TinyDBPageOwnershipStats* stats,
                                 char* message,
                                 size_t message_size);

/* Catalog-wide counterpart to tinydb_check_table_tree(). Every root is
 * structurally walked exactly once, page ownership is checked exactly once,
 * and results are published only after the complete catalog passes. */
static inline bool tinydb_check_catalog_trees(
    Table* table,
    TinyDBCatalogTreeCheck* result,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (table == NULL || result == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid catalog tree-check arguments");
        }
        return false;
    }
    if (table->catalog.num_tables == 0u) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "catalog contains no table roots");
        }
        return false;
    }

    TinyDBCatalogTreeCheck checked;
    memset(&checked, 0, sizeof(checked));
    checked.table_count = table->catalog.num_tables;

    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        const char* table_name = table->catalog.schemas[i].name;
        char tree_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
        if (!tinydb_get_tree_stats_diagnostic(table,
                                               table_name,
                                               &checked.table_stats[i],
                                               tree_message,
                                               sizeof(tree_message))) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "table '%s': %s",
                         table_name,
                         tree_message[0] != '\0'
                             ? tree_message
                             : "tree validation failed");
            }
            return false;
        }
    }

    char ownership_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     &checked.ownership,
                                     ownership_message,
                                     sizeof(ownership_message))) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message[0] != '\0'
                         ? ownership_message
                         : "ownership validation failed");
        }
        return false;
    }

    *result = checked;
    if (message != NULL && message_size > 0u) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u owned=%u free=%u",
                 checked.table_count,
                 checked.ownership.owned_pages,
                 checked.ownership.free_pages);
    }
    return true;
}

bool tinydb_check_database(Table* table,
                           TinyDBPageOwnershipStats* ownership_stats,
                           char* message,
                           size_t message_size);

/*
 * Source-level inspection seam for .btree/.page callers that include the
 * diagnostics API. The historical table.c symbols remain available for ABI
 * compatibility, but diagnostics-aware source callers are routed through
 * non-fatal existing-page try-pin acquisition instead of get_page().
 *
 * Tree traversal snapshots child/key metadata and releases the current node
 * before descending, so inspection needs at most one replaceable frame. A
 * fully pinned pool reports BUSY and returns to the caller rather than taking
 * the process down through lru_evict(). Leaf decoding is routed through the
 * shared V1/V2 leaf-page access boundary rather than the fixed V1 layout.
 */
static inline void tinydb_inspect_indent(uint32_t level) {
    for (uint32_t i = 0u; i < level; i++) printf("  ");
}

static inline bool tinydb_print_tree_nonfatal_impl(Pager* pager,
                                                    uint32_t page_num,
                                                    uint32_t indentation_level) {
    PagerPageHandle handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, page_num, &handle);
    if (status != PAGER_TRY_PIN_OK) {
        printf("Error: Unable to inspect B+ tree page %u: %s.\n",
               page_num,
               pager_try_pin_status_string(status));
        return false;
    }

    void* node = handle.data;
    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t num_keys = 0u;
        if (!tinydb_leaf_page_count(node, PAGE_SIZE, &num_keys)) {
            (void)pager_release_page_handle(&handle);
            printf("Error: Unable to inspect B+ tree page %u: invalid leaf format.\n",
                   page_num);
            return false;
        }

        tinydb_inspect_indent(indentation_level);
        printf("- leaf (size %u)\n", num_keys);
        for (uint32_t i = 0u; i < num_keys; i++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(node, PAGE_SIZE, i, &key)) {
                (void)pager_release_page_handle(&handle);
                printf("Error: Unable to inspect B+ tree page %u: invalid leaf key at cell %u.\n",
                       page_num,
                       i);
                return false;
            }
            tinydb_inspect_indent(indentation_level + 1u);
            printf("- %u\n", key);
        }
        if (!pager_release_page_handle(&handle)) {
            printf("Error: Unable to release B+ tree inspection page %u.\n",
                   page_num);
            return false;
        }
        return true;
    }

    if (type != NODE_INTERNAL) {
        (void)pager_release_page_handle(&handle);
        printf("Error: Unable to inspect B+ tree page %u: unknown node type %u.\n",
               page_num,
               (uint32_t)type);
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        (void)pager_release_page_handle(&handle);
        printf("Error: Unable to inspect B+ tree page %u: invalid internal key count %u.\n",
               page_num,
               num_keys);
        return false;
    }

    uint32_t child_count = num_keys + 1u;
    uint32_t* children = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    uint32_t* keys = num_keys == 0u
        ? NULL
        : (uint32_t*)malloc(sizeof(uint32_t) * num_keys);
    if (children == NULL || (num_keys != 0u && keys == NULL)) {
        free(children);
        free(keys);
        (void)pager_release_page_handle(&handle);
        printf("Error: Unable to allocate B+ tree inspection snapshot for page %u.\n",
               page_num);
        return false;
    }

    for (uint32_t i = 0u; i < num_keys; i++) {
        children[i] = *internal_node_child(node, i);
        keys[i] = *internal_node_key(node, i);
    }
    children[num_keys] = *internal_node_right_child(node);

    tinydb_inspect_indent(indentation_level);
    printf("- internal (size %u)\n", num_keys);
    if (!pager_release_page_handle(&handle)) {
        free(children);
        free(keys);
        printf("Error: Unable to release B+ tree inspection page %u.\n",
               page_num);
        return false;
    }

    for (uint32_t i = 0u; i < num_keys; i++) {
        if (!tinydb_print_tree_nonfatal_impl(pager,
                                             children[i],
                                             indentation_level + 1u)) {
            free(children);
            free(keys);
            return false;
        }
        tinydb_inspect_indent(indentation_level + 1u);
        printf("- key %u\n", keys[i]);
    }
    bool ok = tinydb_print_tree_nonfatal_impl(pager,
                                               children[num_keys],
                                               indentation_level + 1u);
    free(children);
    free(keys);
    return ok;
}

static inline bool tinydb_print_tree_nonfatal(Pager* pager,
                                               uint32_t page_num,
                                               uint32_t indentation_level) {
    if (pager == NULL || page_num >= pager->num_pages) {
        printf("Error: Unable to inspect B+ tree page %u: invalid page.\n",
               page_num);
        return false;
    }
    return tinydb_print_tree_nonfatal_impl(pager,
                                           page_num,
                                           indentation_level);
}

static inline bool tinydb_print_page_nonfatal(Table* table,
                                               uint32_t page_num) {
    if (table == NULL || table->pager == NULL ||
        page_num >= table->pager->num_pages) {
        printf("Error: Page number %u out of bounds (total pages: %u).\n",
               page_num,
               table != NULL && table->pager != NULL
                   ? table->pager->num_pages
                   : 0u);
        return false;
    }

    PagerPageHandle handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        table->pager, page_num, &handle);
    if (status != PAGER_TRY_PIN_OK) {
        printf("Error: Unable to inspect page %u: %s.\n",
               page_num,
               pager_try_pin_status_string(status));
        return false;
    }

    void* page = handle.data;
    NodeType type = get_node_type(page);
    bool is_root_page = is_node_root(page);
    uint32_t parent = *node_parent(page);

    printf("--- Page %u Details ---\n", page_num);
    printf("Type: %s\n",
           type == NODE_LEAF ? "LEAF" :
           type == NODE_INTERNAL ? "INTERNAL" : "UNKNOWN");
    printf("Is Root: %s\n", is_root_page ? "Yes" : "No");
    printf("Parent Page: %u\n", parent);

    bool valid = true;
    if (type == NODE_LEAF) {
        uint32_t num_cells = 0u;
        uint32_t next_leaf = 0u;
        uint32_t prev_leaf = 0u;
        if (!tinydb_leaf_page_count(page, PAGE_SIZE, &num_cells) ||
            !tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_leaf) ||
            !tinydb_leaf_page_next(page, PAGE_SIZE, &next_leaf)) {
            printf("Error: Page %u has invalid leaf format.\n", page_num);
            valid = false;
        } else {
            printf("Leaf Format: %s\n",
                   tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)
                       ? "FIXED_V1"
                       : "SLOTTED_V2");
            printf("Num Cells: %u\n", num_cells);
            printf("Prev Leaf Page: %u\n", prev_leaf);
            printf("Next Leaf Page: %u\n", next_leaf);
            printf("Keys: ");
            for (uint32_t i = 0u; i < num_cells; i++) {
                uint32_t key = 0u;
                if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, i, &key)) {
                    printf("<invalid cell %u>", i);
                    valid = false;
                    break;
                }
                printf("%u%s",
                       key,
                       (i + 1u < num_cells) ? ", " : "");
            }
            printf("\n");
        }
    } else if (type == NODE_INTERNAL) {
        uint32_t num_keys = *internal_node_num_keys(page);
        if (num_keys > INTERNAL_NODE_MAX_KEYS) {
            printf("Error: Page %u has invalid internal key count %u.\n",
                   page_num,
                   num_keys);
            valid = false;
        } else {
            uint32_t right_child = *internal_node_right_child(page);
            printf("Num Keys: %u\n", num_keys);
            printf("Right Child Page: %u\n", right_child);
            printf("Keys & Children:\n");
            for (uint32_t i = 0u; i < num_keys; i++) {
                printf("  Child[%u] -> Page %u | Key[%u] = %u\n",
                       i,
                       *internal_node_child(page, i),
                       i,
                       *internal_node_key(page, i));
            }
            printf("  Child[%u] (Rightmost) -> Page %u\n",
                   num_keys,
                   right_child);
        }
    } else {
        printf("Error: Page %u has unknown node type %u.\n",
               page_num,
               (uint32_t)type);
        valid = false;
    }

    if (!pager_release_page_handle(&handle)) {
        printf("Error: Unable to release page inspection pin for page %u.\n",
               page_num);
        return false;
    }
    return valid;
}

/* Source-built diagnostics consumers, including the REPL, receive the
 * non-fatal inspection implementation. table.c retains the historical ABI
 * symbols for callers that do not include diagnostics.h. */
#define print_tree tinydb_print_tree_nonfatal
#define print_page tinydb_print_page_nonfatal

#endif /* DIAGNOSTICS_H */