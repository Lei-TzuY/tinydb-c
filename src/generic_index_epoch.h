#ifndef GENERIC_INDEX_EPOCH_H
#define GENERIC_INDEX_EPOCH_H

#include "table.h"

#include <stdint.h>

bool tinydb_generic_index_epoch_current(Table* table, uint64_t* epoch);
bool tinydb_generic_index_epoch_before_mutation(Table* table,
                                                const TableSchema* schema);

#endif /* GENERIC_INDEX_EPOCH_H */
