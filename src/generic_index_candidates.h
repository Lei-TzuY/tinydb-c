#ifndef GENERIC_INDEX_CANDIDATES_H
#define GENERIC_INDEX_CANDIDATES_H

#include "generic_predicate.h"
#include "record.h"

#include <stddef.h>

typedef struct {
    uint32_t* ids;
    uint32_t count;
} TinyDBGenericIndexCandidates;

bool tinydb_generic_index_collect_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size);

void tinydb_generic_index_candidates_free(
    TinyDBGenericIndexCandidates* candidates);

#endif /* GENERIC_INDEX_CANDIDATES_H */
