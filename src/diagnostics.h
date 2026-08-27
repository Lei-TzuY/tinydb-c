#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "table.h"

#define TINYDB_DIAGNOSTIC_MESSAGE_MAX 256

typedef struct {
    uint32_t root_page_num;
    uint32_t total_rows;
    uint32_t leaf_pages;
    uint32_t internal_pages;
    uint32_t height;
} TinyDBTreeStats;

const TableSchema* tinydb_find_table_schema(const Table* table, const char* table_name);

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats);

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size);

#endif /* DIAGNOSTICS_H */
