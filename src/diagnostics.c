#include "diagnostics.h"

#include <stdarg.h>

static bool name_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

const TableSchema* tinydb_find_table_schema(const Table* table,
                                            const char* table_name) {
    if (table == NULL || table_name == NULL || table_name[0] == '\0') {
        return NULL;
    }
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (name_equal(table->catalog.schemas[i].name, table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

typedef struct {
    Table* table;
    uint32_t root_page_num;
    bool* visited;
    TinyDBTreeStats* stats;
    char* message;
    size_t message_size;
} TreeWalkContext;

static bool diagnostic_fail(TreeWalkContext* context, const char* format, ...) {
    if (context->message != NULL && context->message_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(context->message, context->message_size, format, args);
        va_end(args);
    }
    return false;
}

static bool page_is_free(Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static bool walk_tree(TreeWalkContext* context,
                      uint32_t page_num,
                      uint32_t expected_parent,
                      uint32_t depth) {
    Pager* pager = context->table->pager;
    if (page_num >= pager->num_pages) {
        return diagnostic_fail(context,
                               "page %u is outside the database page range",
                               page_num);
    }
    if (page_is_free(pager, page_num)) {
        return diagnostic_fail(context,
                               "tree references free page %u",
                               page_num);
    }
    if (context->visited[page_num]) {
        return diagnostic_fail(context,
                               "tree contains a cycle or duplicate child reference at page %u",
                               page_num);
    }
    context->visited[page_num] = true;

    void* node = get_page(pager, page_num);
    bool is_root_page = page_num == context->root_page_num;
    if (is_root_page) {
        if (!is_node_root(node)) {
            return diagnostic_fail(context,
                                   "root page %u is not marked as root",
                                   page_num);
        }
    } else {
        if (is_node_root(node)) {
            return diagnostic_fail(context,
                                   "non-root page %u is marked as root",
                                   page_num);
        }
        if (*node_parent(node) != expected_parent) {
            return diagnostic_fail(context,
                                   "page %u parent is %u, expected %u",
                                   page_num,
                                   *node_parent(node),
                                   expected_parent);
        }
    }

    if (depth + 1 > context->stats->height) {
        context->stats->height = depth + 1;
    }

    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (num_cells > LEAF_NODE_MAX_CELLS) {
            return diagnostic_fail(context,
                                   "leaf page %u has %u cells, max is %u",
                                   page_num,
                                   num_cells,
                                   (uint32_t)LEAF_NODE_MAX_CELLS);
        }
        for (uint32_t i = 1; i < num_cells; i++) {
            uint32_t previous = *leaf_node_key(node, i - 1);
            uint32_t current = *leaf_node_key(node, i);
            if (previous >= current) {
                return diagnostic_fail(context,
                                       "leaf page %u keys are not strictly ordered (%u >= %u)",
                                       page_num,
                                       previous,
                                       current);
            }
        }

        uint32_t previous_leaf = *leaf_node_prev_leaf(node);
        uint32_t next_leaf = *leaf_node_next_leaf(node);
        if (previous_leaf != 0) {
            if (previous_leaf >= pager->num_pages) {
                return diagnostic_fail(context,
                                       "leaf page %u has invalid prev pointer %u",
                                       page_num,
                                       previous_leaf);
            }
            void* previous_node = get_page(pager, previous_leaf);
            if (get_node_type(previous_node) != NODE_LEAF ||
                *leaf_node_next_leaf(previous_node) != page_num) {
                return diagnostic_fail(context,
                                       "leaf page %u has a broken prev link to %u",
                                       page_num,
                                       previous_leaf);
            }
        }
        if (next_leaf != 0) {
            if (next_leaf >= pager->num_pages) {
                return diagnostic_fail(context,
                                       "leaf page %u has invalid next pointer %u",
                                       page_num,
                                       next_leaf);
            }
            void* next_node = get_page(pager, next_leaf);
            if (get_node_type(next_node) != NODE_LEAF ||
                *leaf_node_prev_leaf(next_node) != page_num) {
                return diagnostic_fail(context,
                                       "leaf page %u has a broken next link to %u",
                                       page_num,
                                       next_leaf);
            }
        }

        context->stats->leaf_pages++;
        context->stats->total_rows += num_cells;
        return true;
    }

    if (type != NODE_INTERNAL) {
        return diagnostic_fail(context,
                               "page %u has unknown node type %u",
                               page_num,
                               (uint32_t)type);
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        return diagnostic_fail(context,
                               "internal page %u has %u keys, max is %u",
                               page_num,
                               num_keys,
                               (uint32_t)INTERNAL_NODE_MAX_KEYS);
    }
    for (uint32_t i = 1; i < num_keys; i++) {
        uint32_t previous = *internal_node_key(node, i - 1);
        uint32_t current = *internal_node_key(node, i);
        if (previous >= current) {
            return diagnostic_fail(context,
                                   "internal page %u separator keys are not strictly ordered (%u >= %u)",
                                   page_num,
                                   previous,
                                   current);
        }
    }

    context->stats->internal_pages++;
    for (uint32_t i = 0; i <= num_keys; i++) {
        uint32_t child_page = *internal_node_child(node, i);
        if (!walk_tree(context, child_page, page_num, depth + 1)) {
            return false;
        }
    }
    return true;
}

static bool run_tree_walk(Table* table,
                          const char* table_name,
                          TinyDBTreeStats* stats,
                          char* message,
                          size_t message_size) {
    const TableSchema* schema = tinydb_find_table_schema(table, table_name);
    if (schema == NULL) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "table '%s' not found", table_name);
        }
        return false;
    }
    if (schema->root_page_num >= table->pager->num_pages) {
        if (message != NULL && message_size > 0) {
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
    if (message != NULL && message_size > 0) message[0] = '\0';

    bool* visited = (bool*)calloc(table->pager->num_pages, sizeof(bool));
    if (visited == NULL) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "unable to allocate diagnostic visited map");
        }
        return false;
    }

    TreeWalkContext context;
    context.table = table;
    context.root_page_num = schema->root_page_num;
    context.visited = visited;
    context.stats = stats;
    context.message = message;
    context.message_size = message_size;

    bool ok = walk_tree(&context, schema->root_page_num, schema->root_page_num, 0);
    free(visited);
    return ok;
}

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats) {
    if (table == NULL || table_name == NULL || stats == NULL) return false;
    char ignored[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    return run_tree_walk(table, table_name, stats, ignored, sizeof(ignored));
}

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size) {
    if (table == NULL || table_name == NULL) return false;
    TinyDBTreeStats stats;
    bool ok = run_tree_walk(table, table_name, &stats, message, message_size);
    if (ok && message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "ok: root=%u height=%u rows=%u leaf_pages=%u internal_pages=%u",
                 stats.root_page_num,
                 stats.height,
                 stats.total_rows,
                 stats.leaf_pages,
                 stats.internal_pages);
    }
    return ok;
}
