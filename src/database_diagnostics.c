#include "diagnostics.h"
#include "leaf_page_access.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static bool page_is_free(Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

typedef struct {
    Table* table;
    uint32_t root_page_num;
    bool* visited;
} DatabaseTreeWalk;

static bool reciprocal_leaf_link_ok(DatabaseTreeWalk* walk,
                                    uint32_t page_num,
                                    uint32_t sibling_page_num,
                                    bool sibling_is_previous) {
    if (sibling_page_num == 0u) return true;
    Pager* pager = walk->table->pager;
    if (sibling_page_num >= pager->num_pages ||
        page_is_free(pager, sibling_page_num)) {
        return false;
    }
    void* sibling = get_page(pager, sibling_page_num);
    if (get_node_type(sibling) != NODE_LEAF) return false;
    uint32_t reciprocal = 0u;
    bool readable = sibling_is_previous
        ? tinydb_leaf_page_next(sibling, PAGE_SIZE, &reciprocal)
        : tinydb_leaf_page_prev(sibling, PAGE_SIZE, &reciprocal);
    return readable && reciprocal == page_num;
}

static bool walk_tree_dual(DatabaseTreeWalk* walk,
                           uint32_t page_num,
                           uint32_t expected_parent) {
    Pager* pager = walk->table->pager;
    if (page_num >= pager->num_pages || page_is_free(pager, page_num) ||
        walk->visited[page_num]) {
        return false;
    }
    walk->visited[page_num] = true;

    void* node = get_page(pager, page_num);
    bool root_page = page_num == walk->root_page_num;
    if (root_page) {
        if (!is_node_root(node)) return false;
    } else {
        if (is_node_root(node) || *node_parent(node) != expected_parent) {
            return false;
        }
    }

    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t count = 0u;
        if (!tinydb_leaf_page_count(node, PAGE_SIZE, &count)) return false;
        uint32_t prior_key = 0u;
        for (uint32_t i = 0u; i < count; i++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(node, PAGE_SIZE, i, &key) ||
                (i > 0u && prior_key >= key)) {
                return false;
            }
            prior_key = key;
        }

        uint32_t previous_leaf = 0u;
        uint32_t next_leaf = 0u;
        if (!tinydb_leaf_page_prev(node, PAGE_SIZE, &previous_leaf) ||
            !tinydb_leaf_page_next(node, PAGE_SIZE, &next_leaf) ||
            !reciprocal_leaf_link_ok(walk,
                                     page_num,
                                     previous_leaf,
                                     true) ||
            !reciprocal_leaf_link_ok(walk,
                                     page_num,
                                     next_leaf,
                                     false)) {
            return false;
        }
        return true;
    }

    if (type != NODE_INTERNAL) return false;
    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) return false;
    for (uint32_t i = 1u; i < num_keys; i++) {
        if (*internal_node_key(node, i - 1u) >= *internal_node_key(node, i)) {
            return false;
        }
    }

    uint32_t child_count = num_keys + 1u;
    uint32_t* children = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    if (children == NULL) return false;
    for (uint32_t i = 0u; i < child_count; i++) {
        children[i] = *internal_node_child(node, i);
    }

    bool ok = true;
    for (uint32_t i = 0u; i < child_count; i++) {
        if (!walk_tree_dual(walk, children[i], page_num)) {
            ok = false;
            break;
        }
    }
    free(children);
    return ok;
}

static bool validate_tree_dual(Table* table, const TableSchema* schema) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }
    bool* visited = (bool*)calloc(table->pager->num_pages, sizeof(bool));
    if (visited == NULL) return false;
    DatabaseTreeWalk walk;
    walk.table = table;
    walk.root_page_num = schema->root_page_num;
    walk.visited = visited;
    bool ok = walk_tree_dual(&walk,
                             schema->root_page_num,
                             schema->root_page_num);
    free(visited);
    return ok;
}

bool tinydb_check_database(Table* table,
                           TinyDBPageOwnershipStats* ownership_stats,
                           char* message,
                           size_t message_size) {
    if (table == NULL || table->pager == NULL || ownership_stats == NULL) {
        set_message(message, message_size, "invalid database diagnostic input");
        return false;
    }

    memset(ownership_stats, 0, sizeof(*ownership_stats));
    if (message != NULL && message_size > 0) message[0] = '\0';

    if (table->catalog.num_tables == 0) {
        set_message(message, message_size, "catalog contains no table roots");
        return false;
    }

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* schema = &table->catalog.schemas[i];
        for (uint32_t j = 0; j < i; j++) {
            const TableSchema* previous = &table->catalog.schemas[j];
            if (previous->root_page_num == schema->root_page_num) {
                if (message != NULL && message_size > 0) {
                    snprintf(message,
                             message_size,
                             "tables '%s' and '%s' share root page %u",
                             previous->name,
                             schema->name,
                             schema->root_page_num);
                }
                return false;
            }
        }

        if (!validate_tree_dual(table, schema)) {
            if (message != NULL && message_size > 0) {
                snprintf(message,
                         message_size,
                         "table '%s' B+ tree structural validation failed",
                         schema->name);
            }
            return false;
        }
    }

    char ownership_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     ownership_stats,
                                     ownership_message,
                                     sizeof(ownership_message))) {
        if (message != NULL && message_size > 0) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message);
        }
        return false;
    }

    if (message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u total=%u owned=%u free=%u orphan=%u shared=%u",
                 table->catalog.num_tables,
                 ownership_stats->total_pages,
                 ownership_stats->owned_pages,
                 ownership_stats->free_pages,
                 ownership_stats->orphan_pages,
                 ownership_stats->shared_pages);
    }
    return true;
}
