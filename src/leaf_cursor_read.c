#include "leaf_page_access.h"
#include "table.h"

#include <stdlib.h>

static Cursor* make_cursor(Table* table,
                           uint32_t page_num,
                           uint32_t cell_num,
                           bool end_of_table) {
    Cursor* cursor = (Cursor*)malloc(sizeof(Cursor));
    if (cursor == NULL) return NULL;
    cursor->table = table;
    cursor->page_num = page_num;
    cursor->cell_num = cell_num;
    cursor->end_of_table = end_of_table;
    return cursor;
}

static bool valid_page_number(uint32_t page_num) {
    return page_num != INVALID_PAGE_NUM;
}

static Cursor* leaf_find(Table* table, uint32_t page_num, uint32_t key) {
    if (table == NULL || table->pager == NULL || !valid_page_number(page_num)) {
        return NULL;
    }
    void* page = get_page(table->pager, page_num);
    uint32_t cell_index = 0u;
    bool exact_match = false;
    if (!tinydb_leaf_page_lower_bound(page,
                                      PAGE_SIZE,
                                      key,
                                      &cell_index,
                                      &exact_match)) {
        return NULL;
    }
    (void)exact_match;
    return make_cursor(table, page_num, cell_index, false);
}

static Cursor* internal_find(Table* table, uint32_t page_num, uint32_t key) {
    if (table == NULL || table->pager == NULL || !valid_page_number(page_num)) {
        return NULL;
    }
    void* node = get_page(table->pager, page_num);
    if (get_node_type(node) != NODE_INTERNAL) return NULL;

    uint32_t child_page = *internal_node_find_child(node, key);
    if (!valid_page_number(child_page)) return NULL;
    void* child = get_page(table->pager, child_page);
    if (get_node_type(child) == NODE_LEAF) {
        return leaf_find(table, child_page, key);
    }
    if (get_node_type(child) == NODE_INTERNAL) {
        return internal_find(table, child_page, key);
    }
    return NULL;
}

Cursor* table_find(Table* table, uint32_t key) {
    if (table == NULL || table->pager == NULL ||
        !valid_page_number(table->root_page_num)) {
        return NULL;
    }
    void* root = get_page(table->pager, table->root_page_num);
    if (get_node_type(root) == NODE_LEAF) {
        return leaf_find(table, table->root_page_num, key);
    }
    if (get_node_type(root) == NODE_INTERNAL) {
        return internal_find(table, table->root_page_num, key);
    }
    return NULL;
}

Cursor* table_start(Table* table) {
    Cursor* cursor = table_find(table, 0u);
    if (cursor == NULL) return NULL;
    void* page = get_page(table->pager, cursor->page_num);
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count)) {
        free(cursor);
        return NULL;
    }
    cursor->end_of_table = count == 0u;
    return cursor;
}

void* cursor_value(Cursor* cursor) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL || !valid_page_number(cursor->page_num)) {
        return NULL;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    const void* value = NULL;
    uint32_t value_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &value_length)) {
        return NULL;
    }
    (void)value_length;
    return (void*)value;
}

void cursor_advance(Cursor* cursor) {
    if (cursor == NULL || cursor->end_of_table ||
        cursor->table == NULL || cursor->table->pager == NULL) {
        return;
    }

    while (true) {
        if (!valid_page_number(cursor->page_num)) {
            cursor->end_of_table = true;
            return;
        }
        void* page = get_page(cursor->table->pager, cursor->page_num);
        uint32_t count = 0u;
        if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count)) {
            cursor->end_of_table = true;
            return;
        }

        cursor->cell_num++;
        if (cursor->cell_num < count) return;

        uint32_t next_page = 0u;
        if (!tinydb_leaf_page_next(page, PAGE_SIZE, &next_page) ||
            next_page == 0u || !valid_page_number(next_page)) {
            cursor->end_of_table = true;
            return;
        }
        cursor->page_num = next_page;
        cursor->cell_num = UINT32_MAX;
    }
}

Cursor* table_end(Table* table) {
    if (table == NULL || table->pager == NULL ||
        !valid_page_number(table->root_page_num)) {
        return NULL;
    }

    uint32_t page_num = table->root_page_num;
    void* node = get_page(table->pager, page_num);
    while (get_node_type(node) == NODE_INTERNAL) {
        page_num = *internal_node_right_child(node);
        if (!valid_page_number(page_num)) return NULL;
        node = get_page(table->pager, page_num);
    }
    if (get_node_type(node) != NODE_LEAF) return NULL;

    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(node, PAGE_SIZE, &count)) return NULL;
    return make_cursor(table,
                       page_num,
                       count > 0u ? count - 1u : 0u,
                       count == 0u);
}

void cursor_retreat(Cursor* cursor) {
    if (cursor == NULL || cursor->end_of_table ||
        cursor->table == NULL || cursor->table->pager == NULL) {
        return;
    }
    if (cursor->cell_num > 0u) {
        cursor->cell_num--;
        return;
    }

    while (true) {
        if (!valid_page_number(cursor->page_num)) {
            cursor->end_of_table = true;
            return;
        }
        void* page = get_page(cursor->table->pager, cursor->page_num);
        uint32_t prev_page = 0u;
        if (!tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_page) ||
            prev_page == 0u || !valid_page_number(prev_page)) {
            cursor->end_of_table = true;
            return;
        }

        cursor->page_num = prev_page;
        page = get_page(cursor->table->pager, prev_page);
        uint32_t count = 0u;
        if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count)) {
            cursor->end_of_table = true;
            return;
        }
        if (count > 0u) {
            cursor->cell_num = count - 1u;
            return;
        }
    }
}
