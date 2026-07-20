#include "vm.h"

static void commit_table_changes(Table* table) {
    table_prepare_username_index_commit(table);
    pager_commit(table->pager);
    table_finalize_username_index_commit(table);
}

static bool row_matches_username_filter(Row* row, SelectStatement* sel) {
    return !sel->has_username_filter || strcmp(row->username, sel->username) == 0;
}

static bool fetch_row_by_id(Table* table, uint32_t id, Row* row) {
    Cursor* cursor = table_find(table, id);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    bool found = cursor->cell_num < num_cells &&
                 *leaf_node_key(node, cursor->cell_num) == id;
    if (found) {
        deserialize_row(cursor_value(cursor), row);
    }
    free(cursor);
    return found;
}

static uint32_t username_index_lower_bound(Table* table, const char* username) {
    uint32_t min = 0;
    uint32_t max = table->username_index_count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(table->username_index_entries[mid].username, username) < 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static uint32_t username_index_upper_bound(Table* table, const char* username) {
    uint32_t min = 0;
    uint32_t max = table->username_index_count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(table->username_index_entries[mid].username, username) <= 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static ExecuteResult execute_select_username_index(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;
    table_ensure_username_index(table);

    uint32_t start = username_index_lower_bound(table, sel->username);
    uint32_t end = username_index_upper_bound(table, sel->username);
    uint32_t emitted = 0;
    uint32_t agg_val = 0;
    Row row;

    if (sel->has_order_desc && sel->aggregate == AGGREGATE_NONE) {
        for (uint32_t i = end; i > start; i--) {
            if (sel->has_limit && emitted >= sel->limit) break;
            uint32_t id = table->username_index_entries[i - 1].id;
            if (fetch_row_by_id(table, id, &row)) {
                print_row(&row);
                emitted++;
            }
        }
        return EXECUTE_SUCCESS;
    }

    for (uint32_t i = start; i < end; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        uint32_t id = table->username_index_entries[i].id;
        if (!fetch_row_by_id(table, id, &row)) {
            continue;
        }

        switch (sel->aggregate) {
            case AGGREGATE_NONE:
                print_row(&row);
                break;
            case AGGREGATE_MIN:
                if (emitted == 0) agg_val = id;
                break;
            case AGGREGATE_MAX:
                agg_val = id;
                break;
            default:
                break;
        }
        emitted++;
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT:
            printf("%u\n", emitted);
            break;
        case AGGREGATE_MIN:
        case AGGREGATE_MAX:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        default:
            break;
    }

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_insert(Statement* statement, Table* table) {
    Row* row_to_insert = &(statement->row_to_insert);
    uint32_t key_to_insert = row_to_insert->id;
    
    Cursor* cursor = table_find(table, key_to_insert);

    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if (key_at_index == key_to_insert) {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    table_mark_username_index_dirty(table);

    if (!table->in_transaction) {
        commit_table_changes(table);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

    if (sel->has_username_filter && table->username_index_enabled) {
        return execute_select_username_index(statement, table);
    }

    /* ── DESC path: reverse scan via prev_leaf chain ── */
    if (sel->has_order_desc && sel->aggregate == AGGREGATE_NONE) {
        /* Seek to the starting position:
         *   - No upper bound (no filter, or GT/GTE only): rightmost leaf.
         *   - Upper bound (LT/LTE/EQ): largest key satisfying the bound.  */
        Cursor* cursor;
        if (!sel->has_id_filter ||
            sel->id_op == COMPARE_GT || sel->id_op == COMPARE_GTE) {
            cursor = table_end(table);
        } else {
            /* Find first key >= sel->id, then retreat if needed. */
            cursor = table_find(table, sel->id);
            if (!cursor->end_of_table) {
                void*    node = get_page(table->pager, cursor->page_num);
                uint32_t key  = *leaf_node_key(node, cursor->cell_num);
                bool beyond = (sel->id_op == COMPARE_LT  && key >= sel->id) ||
                              (sel->id_op == COMPARE_LTE && key >  sel->id);
                if (beyond) cursor_retreat(cursor);
            } else {
                /* table_find walked past end — start from rightmost. */
                free(cursor);
                cursor = table_end(table);
            }
        }

        uint32_t emitted = 0;
        Row row;
        while (!cursor->end_of_table) {
            if (sel->has_limit && emitted >= sel->limit) break;

            void*    node = get_page(table->pager, cursor->page_num);
            uint32_t key  = *leaf_node_key(node, cursor->cell_num);

            /* Stop when the current key falls outside the lower bound. */
            if (sel->has_id_filter) {
                if ((sel->id_op == COMPARE_GT  && key <= sel->id) ||
                    (sel->id_op == COMPARE_GTE && key <  sel->id) ||
                    (sel->id_op == COMPARE_EQ  && key <  sel->id))
                    break;
            }

            deserialize_row(cursor_value(cursor), &row);
            if (row_matches_username_filter(&row, sel)) {
                print_row(&row);
                emitted++;
            }
            cursor_retreat(cursor);
        }

        free(cursor);
        return EXECUTE_SUCCESS;
    }

    /* ── Fast path: primary-key point lookup (no aggregate, no LIMIT) ── */
    if (sel->has_id_filter && sel->id_op == COMPARE_EQ &&
        !sel->has_username_filter &&
        sel->aggregate == AGGREGATE_NONE && !sel->has_limit) {
        Cursor* cursor = table_find(table, sel->id);
        void*    node      = get_page(table->pager, cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (cursor->cell_num < num_cells &&
            *leaf_node_key(node, cursor->cell_num) == sel->id) {
            Row row;
            deserialize_row(cursor_value(cursor), &row);
            print_row(&row);
        }
        free(cursor);
        return EXECUTE_SUCCESS;
    }

    /* ── General path: seek to start position ── */
    Cursor* cursor;
    if (!sel->has_id_filter ||
        sel->id_op == COMPARE_LT || sel->id_op == COMPARE_LTE) {
        /* No lower bound or upper-bound-only: start from the first row */
        cursor = table_start(table);
    } else {
        /* EQ, GTE, GT: seek to the first key >= sel->id */
        cursor = table_find(table, sel->id);
        /* GT strictly excludes the equal key */
        if (sel->id_op == COMPARE_GT && !cursor->end_of_table) {
            void* node = get_page(table->pager, cursor->page_num);
            if (cursor->cell_num < *leaf_node_num_cells(node) &&
                *leaf_node_key(node, cursor->cell_num) == sel->id) {
                cursor_advance(cursor);
            }
        }
    }

    uint32_t emitted = 0;
    uint32_t agg_val = 0;
    Row row;

    while (!cursor->end_of_table) {
        /* Check limit before emitting so LIMIT 0 returns nothing */
        if (sel->has_limit && emitted >= sel->limit) break;

        void*    node = get_page(table->pager, cursor->page_num);
        uint32_t key  = *leaf_node_key(node, cursor->cell_num);

        /* Upper-bound check — keys are ascending so we can break early */
        if (sel->has_id_filter) {
            if ((sel->id_op == COMPARE_LT  && key >= sel->id) ||
                (sel->id_op == COMPARE_LTE && key >  sel->id) ||
                (sel->id_op == COMPARE_EQ  && key >  sel->id))
                break;
        }

        if (sel->has_username_filter) {
            deserialize_row(cursor_value(cursor), &row);
            if (!row_matches_username_filter(&row, sel)) {
                cursor_advance(cursor);
                continue;
            }
        }

        switch (sel->aggregate) {
            case AGGREGATE_NONE:
                if (!sel->has_username_filter) {
                    deserialize_row(cursor_value(cursor), &row);
                }
                print_row(&row);
                break;
            case AGGREGATE_MIN:
                if (emitted == 0) agg_val = key; /* first (smallest) key in scan order */
                break;
            case AGGREGATE_MAX:
                agg_val = key; /* always overwrite; last value after loop is largest */
                break;
            default:
                break;
        }
        emitted++;
        cursor_advance(cursor);
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT:
            printf("%u\n", emitted);
            break;
        case AGGREGATE_MIN:
        case AGGREGATE_MAX:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        default:
            break;
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_explain(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

    if (sel->has_username_filter) {
        printf("PLAN: %s (username = '%s')\n",
               table->username_index_enabled ? "SECONDARY INDEX LOOKUP" : "FULL TABLE SCAN",
               sel->username);
    } else if (!sel->has_id_filter) {
        printf("PLAN: FULL TABLE SCAN\n");
    } else if (sel->id_op == COMPARE_EQ) {
        printf("PLAN: PRIMARY KEY LOOKUP (id = %u)\n", sel->id);
    } else {
        const char* op_str = "?";
        switch (sel->id_op) {
            case COMPARE_GT:  op_str = ">";  break;
            case COMPARE_GTE: op_str = ">="; break;
            case COMPARE_LT:  op_str = "<";  break;
            case COMPARE_LTE: op_str = "<="; break;
            default: break;
        }
        printf("PLAN: PRIMARY KEY RANGE SCAN (id %s %u)\n", op_str, sel->id);
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT: printf("      AGGREGATE: COUNT(*)\n"); break;
        case AGGREGATE_MIN:   printf("      AGGREGATE: MIN(id)\n");  break;
        case AGGREGATE_MAX:   printf("      AGGREGATE: MAX(id)\n");  break;
        default: break;
    }
    if (sel->has_order_desc) printf("      ORDER BY id DESC\n");
    if (sel->has_limit)      printf("      LIMIT %u\n", sel->limit);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
    if (statement->delete_all) {
        table_truncate(table);
        table_mark_username_index_dirty(table);
        if (!table->in_transaction) {
            commit_table_changes(table);
        }
        return EXECUTE_SUCCESS;
    }

    uint32_t key = statement->delete_id;
    Cursor* cursor = table_find(table, key);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells ||
        *leaf_node_key(node, cursor->cell_num) != key) {
        free(cursor);
        return EXECUTE_KEY_NOT_FOUND;
    }

    leaf_node_delete(cursor);
    table_mark_username_index_dirty(table);

    if (!table->in_transaction) {
        commit_table_changes(table);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_update(Statement* statement, Table* table) {
    uint32_t key = statement->update.id;
    Cursor* cursor = table_find(table, key);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells ||
        *leaf_node_key(node, cursor->cell_num) != key) {
        free(cursor);
        return EXECUTE_KEY_NOT_FOUND;
    }

    Row row;
    deserialize_row(cursor_value(cursor), &row);

    if (statement->update.set_username) {
        strncpy(row.username, statement->update.username, sizeof(row.username) - 1);
        row.username[sizeof(row.username) - 1] = '\0';
    }
    if (statement->update.set_email) {
        strncpy(row.email, statement->update.email, sizeof(row.email) - 1);
        row.email[sizeof(row.email) - 1] = '\0';
    }

    serialize_row(&row, cursor_value(cursor));
    mark_page_dirty(table->pager, cursor->page_num);
    table_mark_username_index_dirty(table);

    if (!table->in_transaction) {
        commit_table_changes(table);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_begin(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_TRANSACTION_ALREADY_ACTIVE;
    }
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_commit(Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    table->in_transaction = false;
    commit_table_changes(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback(Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    pager_rollback(table->pager);
    table_mark_username_index_dirty(table);
    table->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_create_index(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    table_create_username_index(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_drop_index(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    table_drop_username_index(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
    if (statement->explain) {
        return execute_explain(statement, table);
    }

    switch (statement->type) {
        case (STATEMENT_INSERT):
            return execute_insert(statement, table);
        case (STATEMENT_SELECT):
            return execute_select(statement, table);
        case (STATEMENT_DELETE):
            return execute_delete(statement, table);
        case (STATEMENT_BEGIN):
            return execute_begin(table);
        case (STATEMENT_COMMIT):
            return execute_commit(table);
        case (STATEMENT_ROLLBACK):
            return execute_rollback(table);
        case (STATEMENT_UPDATE):
            return execute_update(statement, table);
        case (STATEMENT_CREATE_INDEX):
            return execute_create_index(table);
        case (STATEMENT_DROP_INDEX):
            return execute_drop_index(table);
    }
    return EXECUTE_SUCCESS;
}
