#include "vm.h"

static void commit_table_changes(Table* table) {
    table_prepare_all_indexes_commit(table);
    pager_commit(table->pager);
    table_finalize_all_indexes_commit(table);
}

static bool like_match(const char* pattern, const char* str) {
    if (*pattern == '\0') return *str == '\0';

    if (*pattern == '%') {
        while (*pattern == '%') pattern++;
        if (*pattern == '\0') return true;
        while (*str != '\0') {
            if (like_match(pattern, str)) return true;
            str++;
        }
        return like_match(pattern, str);
    }

    if (*str == '\0') return false;

    if (*pattern == '_' || *pattern == *str) {
        return like_match(pattern + 1, str + 1);
    }

    return false;
}

static bool row_matches_filters(Row* row, SelectStatement* sel) {
    if (sel->has_id_filter) {
        if (sel->id_op == COMPARE_EQ  && row->id != sel->id) return false;
        if (sel->id_op == COMPARE_GT  && row->id <= sel->id) return false;
        if (sel->id_op == COMPARE_GTE && row->id <  sel->id) return false;
        if (sel->id_op == COMPARE_LT  && row->id >= sel->id) return false;
        if (sel->id_op == COMPARE_LTE && row->id >  sel->id) return false;
    }
    if (sel->has_id_min_filter) {
        if (sel->id_min_op == COMPARE_GT  && row->id <= sel->id_min) return false;
        if (sel->id_min_op == COMPARE_GTE && row->id <  sel->id_min) return false;
    }
    if (sel->has_id_max_filter) {
        if (sel->id_max_op == COMPARE_LT  && row->id >= sel->id_max) return false;
        if (sel->id_max_op == COMPARE_LTE && row->id >  sel->id_max) return false;
    }
    if (sel->has_username_filter && strcmp(row->username, sel->username) != 0) {
        return false;
    }
    if (sel->has_username_like && !like_match(sel->username_like, row->username)) {
        return false;
    }
    if (sel->has_email_filter && strcmp(row->email, sel->email) != 0) {
        return false;
    }
    if (sel->has_email_like && !like_match(sel->email_like, row->email)) {
        return false;
    }
    return true;
}

static bool row_matches_username_filter(Row* row, SelectStatement* sel) {
    return row_matches_filters(row, sel);
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
                if (row_matches_filters(&row, sel)) {
                    print_row(&row);
                    emitted++;
                }
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

        if (!row_matches_filters(&row, sel)) {
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

static uint32_t generic_index_lower_bound(GenericSecondaryIndex* idx, const char* target) {
    uint32_t min = 0;
    uint32_t max = idx->count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(idx->entries[mid].key_val, target) < 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static uint32_t generic_index_upper_bound(GenericSecondaryIndex* idx, const char* target) {
    uint32_t min = 0;
    uint32_t max = idx->count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(idx->entries[mid].key_val, target) <= 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static ExecuteResult execute_select_generic_index(Statement* statement, Table* table, GenericSecondaryIndex* idx, const char* key_val) {
    SelectStatement* sel = &statement->select;
    table_ensure_all_indexes(table);

    uint32_t start = generic_index_lower_bound(idx, key_val);
    uint32_t end = generic_index_upper_bound(idx, key_val);
    uint32_t emitted = 0;
    uint32_t agg_val = 0;
    Row row;

    if (sel->has_order_desc && sel->aggregate == AGGREGATE_NONE) {
        for (uint32_t i = end; i > start; i--) {
            if (sel->has_limit && emitted >= sel->limit) break;
            uint32_t id = idx->entries[i - 1].primary_key;
            if (fetch_row_by_id(table, id, &row)) {
                if (row_matches_filters(&row, sel)) {
                    print_row(&row);
                    emitted++;
                }
            }
        }
        return EXECUTE_SUCCESS;
    }

    for (uint32_t i = start; i < end; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        uint32_t id = idx->entries[i].primary_key;
        if (!fetch_row_by_id(table, id, &row)) {
            continue;
        }

        if (!row_matches_filters(&row, sel)) {
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

static ExecuteResult execute_join_select(Statement* statement, Table* table) {
    (void)statement;
    Cursor* c1 = table_start(table);
    uint32_t match_count = 0;
    Row r1, r2;

    while (!c1->end_of_table) {
        deserialize_row(cursor_value(c1), &r1);

        Cursor* c2 = table_start(table);
        while (!c2->end_of_table) {
            deserialize_row(cursor_value(c2), &r2);

            if (r1.id == r2.id) {
                printf("(%u, %s, %s) | (%u, %s, %s)\n",
                       r1.id, r1.username, r1.email,
                       r2.id, r2.username, r2.email);
                match_count++;
            }
            cursor_advance(c2);
        }
        free(c2);
        cursor_advance(c1);
    }
    free(c1);
    return EXECUTE_SUCCESS;
}

typedef struct {
    char key[256];
    uint32_t count;
    uint32_t min_id;
    uint32_t max_id;
    uint64_t sum_id;
} GroupBucket;

static bool eval_having_condition(GroupBucket* b, SelectStatement* sel) {
    if (!sel->has_having) return true;

    uint32_t val = 0;
    switch (sel->having_agg) {
        case AGGREGATE_COUNT: val = b->count; break;
        case AGGREGATE_MIN:   val = b->min_id; break;
        case AGGREGATE_MAX:   val = b->max_id; break;
        case AGGREGATE_SUM:   val = (uint32_t)b->sum_id; break;
        case AGGREGATE_AVG:   val = b->count > 0 ? (uint32_t)(b->sum_id / b->count) : 0; break;
        default: return true;
    }

    switch (sel->having_op) {
        case COMPARE_EQ:  return val == sel->having_val;
        case COMPARE_GT:  return val > sel->having_val;
        case COMPARE_GTE: return val >= sel->having_val;
        case COMPARE_LT:  return val < sel->having_val;
        case COMPARE_LTE: return val <= sel->having_val;
        default: return true;
    }
}

static ExecuteResult execute_select_groupby(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

#define MAX_GROUP_BUCKETS 256
    GroupBucket buckets[MAX_GROUP_BUCKETS];
    uint32_t num_buckets = 0;
    memset(buckets, 0, sizeof(buckets));

    Cursor* cursor = table_start(table);
    Row row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        if (row_matches_filters(&row, sel)) {
            char group_key[256] = "";
            if (strcmp(sel->group_by_col, "email") == 0) {
                strncpy(group_key, row.email, sizeof(group_key) - 1);
            } else if (strcmp(sel->group_by_col, "username") == 0) {
                strncpy(group_key, row.username, sizeof(group_key) - 1);
            } else {
                snprintf(group_key, sizeof(group_key), "%u", row.id);
            }

            int found_idx = -1;
            for (uint32_t i = 0; i < num_buckets; i++) {
                if (strcmp(buckets[i].key, group_key) == 0) {
                    found_idx = (int)i;
                    break;
                }
            }

            if (found_idx < 0) {
                if (num_buckets < MAX_GROUP_BUCKETS) {
                    found_idx = (int)num_buckets++;
                    strncpy(buckets[found_idx].key, group_key, sizeof(buckets[found_idx].key) - 1);
                    buckets[found_idx].count = 0;
                    buckets[found_idx].min_id = row.id;
                    buckets[found_idx].max_id = row.id;
                    buckets[found_idx].sum_id = 0;
                }
            }

            if (found_idx >= 0) {
                GroupBucket* b = &buckets[found_idx];
                b->count++;
                if (row.id < b->min_id) b->min_id = row.id;
                if (row.id > b->max_id) b->max_id = row.id;
                b->sum_id += row.id;
            }
        }
        cursor_advance(cursor);
    }
    free(cursor);

    uint32_t emitted = 0;
    for (uint32_t i = 0; i < num_buckets; i++) {
        GroupBucket* b = &buckets[i];
        if (!eval_having_condition(b, sel)) continue;
        if (sel->has_limit && emitted >= sel->limit) break;

        uint32_t agg_val = 0;
        switch (sel->aggregate) {
            case AGGREGATE_COUNT: agg_val = b->count; break;
            case AGGREGATE_MIN:   agg_val = b->min_id; break;
            case AGGREGATE_MAX:   agg_val = b->max_id; break;
            case AGGREGATE_SUM:   agg_val = (uint32_t)b->sum_id; break;
            case AGGREGATE_AVG:   agg_val = b->count > 0 ? (uint32_t)(b->sum_id / b->count) : 0; break;
            default:              agg_val = b->count; break;
        }

        if (sel->has_project_col) {
            printf("%s | %u\n", b->key, agg_val);
        } else {
            printf("%s: %u\n", b->key, agg_val);
        }
        emitted++;
    }

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;
    if (sel->has_join) {
        return execute_join_select(statement, table);
    }

    if (sel->has_group_by) {
        return execute_select_groupby(statement, table);
    }

    if (sel->has_username_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "username");
        if (idx != NULL && idx->enabled) {
            return execute_select_generic_index(statement, table, idx, sel->username);
        } else if (table->username_index_enabled) {
            return execute_select_username_index(statement, table);
        }
    } else if (sel->has_email_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "email");
        if (idx != NULL && idx->enabled) {
            return execute_select_generic_index(statement, table, idx, sel->email);
        }
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
    uint32_t start_key = 0;
    bool seek_lower = false;
    CompareOp lower_op = COMPARE_GTE;

    if (sel->has_id_min_filter) {
        seek_lower = true;
        start_key = sel->id_min;
        lower_op = sel->id_min_op;
    } else if (sel->has_id_filter && (sel->id_op == COMPARE_EQ || sel->id_op == COMPARE_GT || sel->id_op == COMPARE_GTE)) {
        seek_lower = true;
        start_key = sel->id;
        lower_op = sel->id_op;
    }

    if (seek_lower) {
        cursor = table_find(table, start_key);
        if (lower_op == COMPARE_GT && !cursor->end_of_table) {
            void* node = get_page(table->pager, cursor->page_num);
            if (cursor->cell_num < *leaf_node_num_cells(node) &&
                *leaf_node_key(node, cursor->cell_num) == start_key) {
                cursor_advance(cursor);
            }
        }
    } else {
        cursor = table_start(table);
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
        if (sel->has_id_max_filter) {
            if ((sel->id_max_op == COMPARE_LT  && key >= sel->id_max) ||
                (sel->id_max_op == COMPARE_LTE && key >  sel->id_max))
                break;
        }
        if (sel->has_id_filter) {
            if ((sel->id_op == COMPARE_LT  && key >= sel->id) ||
                (sel->id_op == COMPARE_LTE && key >  sel->id) ||
                (sel->id_op == COMPARE_EQ  && key >  sel->id))
                break;
        }

        if (sel->has_username_filter || sel->has_email_filter) {
            deserialize_row(cursor_value(cursor), &row);
            if (!row_matches_filters(&row, sel)) {
                cursor_advance(cursor);
                continue;
            }
        }

        switch (sel->aggregate) {
            case AGGREGATE_NONE:
                if (!sel->has_username_filter && !sel->has_email_filter) {
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
            case AGGREGATE_SUM:
            case AGGREGATE_AVG:
                agg_val += key;
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
        case AGGREGATE_SUM:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        case AGGREGATE_AVG:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val / emitted);
            break;
        default:
            break;
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_explain(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

    if (sel->has_group_by) {
        printf("PLAN: GROUP BY AGGREGATION SCAN (GROUP BY %s%s)\n",
               sel->group_by_col,
               sel->has_having ? ", HAVING FILTER" : "");
    } else if (sel->has_username_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "username");
        bool idx_enabled = (idx && idx->enabled) || table->username_index_enabled;
        printf("PLAN: %s (username = '%s')\n",
               idx_enabled ? "SECONDARY INDEX LOOKUP" : "FULL TABLE SCAN",
               sel->username);
    } else if (sel->has_email_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "email");
        bool idx_enabled = (idx && idx->enabled);
        printf("PLAN: %s (email = '%s')\n",
               idx_enabled ? "SECONDARY INDEX LOOKUP" : "FULL TABLE SCAN",
               sel->email);
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
        case AGGREGATE_SUM:   printf("      AGGREGATE: SUM(id)\n");  break;
        case AGGREGATE_AVG:   printf("      AGGREGATE: AVG(id)\n");  break;
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

ExecuteResult execute_create_index(Statement* statement, Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    const char* idx_name = statement->create_index.name[0] != '\0' ? statement->create_index.name : "idx_users_username";
    const char* tbl_name = statement->create_index.table_name[0] != '\0' ? statement->create_index.table_name : "users";
    const char* col_name = statement->create_index.column_name[0] != '\0' ? statement->create_index.column_name : "username";

    table_create_index(table, idx_name, tbl_name, col_name);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_drop_index(Statement* statement, Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    const char* idx_name = statement->drop_index.name[0] != '\0' ? statement->drop_index.name : "idx_users_username";
    table_drop_index(table, idx_name);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_vacuum(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    db_vacuum(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_savepoint(Statement* statement, Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    if (!pager_savepoint(table->pager, statement->savepoint.name)) {
        return EXECUTE_SAVEPOINT_STACK_FULL;
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback_to(Statement* statement, Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    if (!pager_rollback_to_savepoint(table->pager, statement->savepoint.name)) {
        return EXECUTE_SAVEPOINT_NOT_FOUND;
    }
    table_mark_username_index_dirty(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_release_savepoint(Statement* statement, Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    if (!pager_release_savepoint(table->pager, statement->savepoint.name)) {
        return EXECUTE_SAVEPOINT_NOT_FOUND;
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_checkpoint(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    db_checkpoint(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_integrity_check(Table* table) {
    db_integrity_check(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_user_version(Table* table) {
    printf("%u\n", db_get_user_version(table));
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_set_user_version(Statement* statement, Table* table) {
    db_set_user_version(table, statement->pragma.user_version);
    if (!table->in_transaction) {
        commit_table_changes(table);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_table_info(Table* table) {
    (void)table;
    printf("cid | name     | type         | notnull | dflt_value | pk\n");
    printf("----+----------+--------------+---------+------------+---\n");
    printf("0   | id       | INT          | 1       | NULL       | 1\n");
    printf("1   | username | VARCHAR(32)  | 0       | NULL       | 0\n");
    printf("2   | email    | VARCHAR(255) | 0       | NULL       | 0\n");
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_index_list(Table* table) {
    table_ensure_all_indexes(table);
    printf("seq | name                 | unique | origin | partial\n");
    printf("----+----------------------+--------+--------+--------\n");
    uint32_t count = 0;
    if (table->username_index_enabled) {
        table_ensure_username_index(table);
        printf("%u   | idx_users_username   | 0      | c      | 0\n", count++);
    }
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        if (table->sec_indexes[i].enabled) {
            if (table->username_index_enabled && strcmp(table->sec_indexes[i].name, "idx_users_username") == 0) continue;
            printf("%u   | %-20s | 0      | c      | 0\n", count++, table->sec_indexes[i].name);
        }
    }
    if (count == 0) {
        printf("(no indexes found)\n");
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_create_table(Statement* statement, Table* table) {
    if (table_create_table(table, statement->create_table.table_name,
                           statement->create_table.num_columns,
                           statement->create_table.col_names,
                           statement->create_table.col_types,
                           statement->create_table.has_fk,
                           statement->create_table.fk_col,
                           statement->create_table.fk_parent_table,
                           statement->create_table.fk_parent_col)) {
        return EXECUTE_SUCCESS;
    }
    return EXECUTE_TABLE_FULL;
}

typedef struct {
    char name[32];
    char sql_template[256];
} PreparedStatementEntry;

static PreparedStatementEntry prepared_statements[16];
static uint32_t prepared_count = 0;

ExecuteResult execute_prepare(Statement* statement) {
    for (uint32_t i = 0; i < prepared_count; i++) {
        if (strcmp(prepared_statements[i].name, statement->prepare.name) == 0) {
            strncpy(prepared_statements[i].sql_template, statement->prepare.sql_template, 255);
            printf("Statement '%s' prepared.\n", statement->prepare.name);
            return EXECUTE_SUCCESS;
        }
    }
    if (prepared_count >= 16) return EXECUTE_TABLE_FULL;
    strncpy(prepared_statements[prepared_count].name, statement->prepare.name, 31);
    strncpy(prepared_statements[prepared_count].sql_template, statement->prepare.sql_template, 255);
    prepared_count++;
    printf("Statement '%s' prepared.\n", statement->prepare.name);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_execute_prepared(Statement* statement, Table* table) {
    const char* template_str = NULL;
    for (uint32_t i = 0; i < prepared_count; i++) {
        if (strcmp(prepared_statements[i].name, statement->execute_prepared.name) == 0) {
            template_str = prepared_statements[i].sql_template;
            break;
        }
    }
    if (template_str == NULL) {
        printf("Error: Prepared statement '%s' not found.\n", statement->execute_prepared.name);
        return EXECUTE_SUCCESS;
    }

    char bound_sql[512];
    const char* p = strchr(template_str, '?');
    if (p != NULL) {
        size_t prefix_len = p - template_str;
        snprintf(bound_sql, sizeof(bound_sql), "%.*s%u%s", (int)prefix_len, template_str, statement->execute_prepared.param_val, p + 1);
    } else {
        strncpy(bound_sql, template_str, sizeof(bound_sql) - 1);
    }

    Statement bound_stmt;
    PrepareResult prep_res = prepare_statement(bound_sql, &bound_stmt);
    if (prep_res == PREPARE_SUCCESS) {
        return execute_statement(&bound_stmt, table);
    }
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
            return execute_create_index(statement, table);
        case (STATEMENT_DROP_INDEX):
            return execute_drop_index(statement, table);
        case (STATEMENT_VACUUM):
            return execute_vacuum(table);
        case (STATEMENT_SAVEPOINT):
            return execute_savepoint(statement, table);
        case (STATEMENT_ROLLBACK_TO):
            return execute_rollback_to(statement, table);
        case (STATEMENT_RELEASE_SAVEPOINT):
            return execute_release_savepoint(statement, table);
        case (STATEMENT_CHECKPOINT):
            return execute_checkpoint(table);
        case (STATEMENT_PRAGMA_INTEGRITY_CHECK):
            return execute_integrity_check(table);
        case (STATEMENT_PRAGMA_USER_VERSION):
            return execute_pragma_user_version(table);
        case (STATEMENT_PRAGMA_SET_USER_VERSION):
            return execute_pragma_set_user_version(statement, table);
        case (STATEMENT_PRAGMA_TABLE_INFO):
            return execute_pragma_table_info(table);
        case (STATEMENT_PRAGMA_INDEX_LIST):
            return execute_pragma_index_list(table);
        case (STATEMENT_CREATE_TABLE):
            return execute_create_table(statement, table);
        case (STATEMENT_PREPARE):
            return execute_prepare(statement);
        case (STATEMENT_EXECUTE_PREPARED):
            return execute_execute_prepared(statement, table);
    }
    return EXECUTE_SUCCESS;
}
