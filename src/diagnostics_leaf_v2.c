#include "diagnostics.h"
#include "leaf_page_access.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Table* table;
    uint32_t root_page_num;
    bool* visited;
    TinyDBTreeStats* stats;
    char* message;
    size_t message_size;
} DualTreeWalkContext;

static bool dual_fail(DualTreeWalkContext* context, const char* format, ...) {
    if (context->message != NULL && context->message_size > 0u) {
        va_list args;
        va_start(args, format);
        vsnprintf(context->message, context->message_size, format, args);
        va_end(args);
    }
    return false;
}

static bool page_is_free(Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static bool validate_leaf_neighbor(DualTreeWalkContext* context,
                                   uint32_t page_num,
                                   uint32_t neighbor_num,
                                   bool previous) {
    Pager* pager = context->table->pager;
    if (neighbor_num == 0u) return true;
    if (neighbor_num >= pager->num_pages) {
        return dual_fail(context,
                         "leaf page %u has invalid %s pointer %u",
                         page_num,
                         previous ? "prev" : "next",
                         neighbor_num);
    }
    if (page_is_free(pager, neighbor_num)) {
        return dual_fail(context,
                         "leaf page %u %s pointer references free page %u",
                         page_num,
                         previous ? "prev" : "next",
                         neighbor_num);
    }

    void* neighbor = get_page(pager, neighbor_num);
    if (get_node_type(neighbor) != NODE_LEAF) {
        return dual_fail(context,
                         "leaf page %u has %s link to non-leaf page %u",
                         page_num,
                         previous ? "prev" : "next",
                         neighbor_num);
    }
    uint32_t reciprocal = 0u;
    bool ok = previous
        ? tinydb_leaf_page_next(neighbor, PAGE_SIZE, &reciprocal)
        : tinydb_leaf_page_prev(neighbor, PAGE_SIZE, &reciprocal);
    if (!ok || reciprocal != page_num) {
        return dual_fail(context,
                         "leaf page %u has a broken %s link to %u",
                         page_num,
                         previous ? "prev" : "next",
                         neighbor_num);
    }
    return true;
}

static bool walk_tree_dual(DualTreeWalkContext* context,
                           uint32_t page_num,
                           uint32_t expected_parent,
                           uint32_t depth) {
    Pager* pager = context->table->pager;
    if (page_num >= pager->num_pages) {
        return dual_fail(context,
                         "page %u is outside the database page range",
                         page_num);
    }
    if (page_is_free(pager, page_num)) {
        return dual_fail(context, "tree references free page %u", page_num);
    }
    if (context->visited[page_num]) {
        return dual_fail(context,
                         "tree contains a cycle or duplicate child reference at page %u",
                         page_num);
    }
    context->visited[page_num] = true;

    void* node = get_page(pager, page_num);
    bool root_page = page_num == context->root_page_num;
    if (root_page) {
        if (!is_node_root(node)) {
            return dual_fail(context,
                             "root page %u is not marked as root",
                             page_num);
        }
    } else {
        if (is_node_root(node)) {
            return dual_fail(context,
                             "non-root page %u is marked as root",
                             page_num);
        }
        if (*node_parent(node) != expected_parent) {
            return dual_fail(context,
                             "page %u parent is %u, expected %u",
                             page_num,
                             *node_parent(node),
                             expected_parent);
        }
    }

    if (depth + 1u > context->stats->height) {
        context->stats->height = depth + 1u;
    }

    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t count = 0u;
        if (!tinydb_leaf_page_count(node, PAGE_SIZE, &count)) {
            return dual_fail(context,
                             "leaf page %u has an invalid or unsupported physical format",
                             page_num);
        }
        uint32_t previous_key = 0u;
        for (uint32_t i = 0u; i < count; i++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(node, PAGE_SIZE, i, &key)) {
                return dual_fail(context,
                                 "leaf page %u has an unreadable cell %u",
                                 page_num,
                                 i);
            }
            if (i > 0u && previous_key >= key) {
                return dual_fail(context,
                                 "leaf page %u keys are not strictly ordered (%u >= %u)",
                                 page_num,
                                 previous_key,
                                 key);
            }
            previous_key = key;
        }

        uint32_t previous_leaf = 0u;
        uint32_t next_leaf = 0u;
        if (!tinydb_leaf_page_prev(node, PAGE_SIZE, &previous_leaf) ||
            !tinydb_leaf_page_next(node, PAGE_SIZE, &next_leaf)) {
            return dual_fail(context,
                             "leaf page %u has unreadable sibling metadata",
                             page_num);
        }
        if (!validate_leaf_neighbor(context, page_num, previous_leaf, true) ||
            !validate_leaf_neighbor(context, page_num, next_leaf, false)) {
            return false;
        }

        context->stats->leaf_pages++;
        context->stats->total_rows += count;
        return true;
    }

    if (type != NODE_INTERNAL) {
        return dual_fail(context,
                         "page %u has unknown node type %u",
                         page_num,
                         (uint32_t)type);
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        return dual_fail(context,
                         "internal page %u has %u keys, max is %u",
                         page_num,
                         num_keys,
                         (uint32_t)INTERNAL_NODE_MAX_KEYS);
    }
    for (uint32_t i = 1u; i < num_keys; i++) {
        uint32_t previous = *internal_node_key(node, i - 1u);
        uint32_t current = *internal_node_key(node, i);
        if (previous >= current) {
            return dual_fail(context,
                             "internal page %u separator keys are not strictly ordered (%u >= %u)",
                             page_num,
                             previous,
                             current);
        }
    }

    uint32_t child_count = num_keys + 1u;
    uint32_t* children = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    if (children == NULL) {
        return dual_fail(context,
                         "unable to allocate child snapshot for page %u",
                         page_num);
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        children[i] = *internal_node_child(node, i);
    }

    context->stats->internal_pages++;
    for (uint32_t i = 0u; i < child_count; i++) {
        if (!walk_tree_dual(context, children[i], page_num, depth + 1u)) {
            free(children);
            return false;
        }
    }
    free(children);
    return true;
}

static bool run_dual_tree_walk(Table* table,
                               const char* table_name,
                               TinyDBTreeStats* stats,
                               char* message,
                               size_t message_size) {
    const TableSchema* schema = tinydb_find_table_schema(table, table_name);
    if (schema == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "table '%s' not found", table_name);
        }
        return false;
    }
    if (schema->root_page_num >= table->pager->num_pages) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "table '%s' root page %u is outside the database",
                     schema->name,
                     schema->root_page_num);
        }
        return false;
    }

    memset(stats, 0, sizeof(*stats));
    stats->root_page_num = schema->root_page_num;
    if (message != NULL && message_size > 0u) message[0] = '\0';

    bool* visited = (bool*)calloc(table->pager->num_pages, sizeof(bool));
    if (visited == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "unable to allocate diagnostic visited map");
        }
        return false;
    }

    DualTreeWalkContext context;
    context.table = table;
    context.root_page_num = schema->root_page_num;
    context.visited = visited;
    context.stats = stats;
    context.message = message;
    context.message_size = message_size;
    bool ok = walk_tree_dual(&context,
                             schema->root_page_num,
                             schema->root_page_num,
                             0u);
    free(visited);
    return ok;
}

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats) {
    if (table == NULL || table_name == NULL || stats == NULL) return false;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    return run_dual_tree_walk(table,
                              table_name,
                              stats,
                              message,
                              sizeof(message));
}

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size) {
    if (table == NULL || table_name == NULL) return false;

    TinyDBTreeStats tree_stats;
    if (!run_dual_tree_walk(table,
                            table_name,
                            &tree_stats,
                            message,
                            message_size)) {
        return false;
    }

    TinyDBPageOwnershipStats ownership;
    char ownership_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     &ownership,
                                     ownership_message,
                                     sizeof(ownership_message))) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message);
        }
        return false;
    }

    if (message != NULL && message_size > 0u) {
        snprintf(message,
                 message_size,
                 "ok: root=%u height=%u rows=%u leaf_pages=%u internal_pages=%u ownership_owned=%u free=%u",
                 tree_stats.root_page_num,
                 tree_stats.height,
                 tree_stats.total_rows,
                 tree_stats.leaf_pages,
                 tree_stats.internal_pages,
                 ownership.owned_pages,
                 ownership.free_pages);
    }
    return true;
}
