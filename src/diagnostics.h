#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "table.h"

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
    uint32_t total_pages;
    uint32_t owned_pages;
    uint32_t free_pages;
    uint32_t orphan_pages;
    uint32_t shared_pages;
} TinyDBPageOwnershipStats;

const TableSchema* tinydb_find_table_schema(const Table* table, const char* table_name);

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats);

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size);

bool tinydb_check_page_ownership(Table* table,
                                 TinyDBPageOwnershipStats* stats,
                                 char* message,
                                 size_t message_size);

#endif /* DIAGNOSTICS_H */
