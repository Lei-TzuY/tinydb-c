#include "generic_index_candidates.h"

#include <stdlib.h>
#include <string.h>

static int compare_u32(const void* left, const void* right) {
    uint32_t a = *(const uint32_t*)left;
    uint32_t b = *(const uint32_t*)right;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static void set_message(char* message, size_t message_size, const char* text) {
    if (message == NULL || message_size == 0) return;
    size_t length = strlen(text);
    if (length >= message_size) length = message_size - 1u;
    memcpy(message, text, length);
    message[length] = '\0';
}

static bool compatible_predicates(const TableSchema* schema,
                                  GenericSecondaryIndex* index,
                                  const TinyDBGenericPredicate* predicates,
                                  uint32_t predicate_count) {
    if (schema == NULL || index == NULL || predicates == NULL || predicate_count == 0) {
        return false;
    }
    uint32_t column_index = predicates[0].column_index;
    if (column_index == 0 || column_index >= schema->num_columns ||
        predicates[0].op == TINYDB_GENERIC_COMPARE_LIKE) {
        return false;
    }
    for (uint32_t i = 1; i < predicate_count; i++) {
        if (predicates[i].column_index != column_index ||
            predicates[i].op == TINYDB_GENERIC_COMPARE_LIKE) {
            return false;
        }
    }
    return true;
}

bool tinydb_generic_index_collect_conjunctive_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size) {
    if (candidates != NULL) memset(candidates, 0, sizeof(*candidates));
    if (table == NULL || candidates == NULL ||
        !compatible_predicates(schema, index, predicates, predicate_count)) {
        set_message(message, message_size, "invalid conjunctive generic index candidate request");
        return false;
    }

    TinyDBGenericIndexCandidates working;
    memset(&working, 0, sizeof(working));
    if (!tinydb_generic_index_collect_candidates(table,
                                                 schema,
                                                 index,
                                                 &predicates[0],
                                                 &working,
                                                 message,
                                                 message_size)) {
        return false;
    }
    if (working.count > 1) {
        qsort(working.ids, working.count, sizeof(uint32_t), compare_u32);
    }

    for (uint32_t predicate_index = 1;
         predicate_index < predicate_count && working.count > 0;
         predicate_index++) {
        TinyDBGenericIndexCandidates source;
        memset(&source, 0, sizeof(source));
        if (!tinydb_generic_index_collect_candidates(table,
                                                     schema,
                                                     index,
                                                     &predicates[predicate_index],
                                                     &source,
                                                     message,
                                                     message_size)) {
            tinydb_generic_index_candidates_free(&working);
            return false;
        }
        if (source.count > 1) {
            qsort(source.ids, source.count, sizeof(uint32_t), compare_u32);
        }

        uint32_t left = 0;
        uint32_t right = 0;
        uint32_t out = 0;
        while (left < working.count && right < source.count) {
            if (working.ids[left] < source.ids[right]) {
                left++;
            } else if (working.ids[left] > source.ids[right]) {
                right++;
            } else {
                working.ids[out++] = working.ids[left];
                left++;
                right++;
            }
        }
        working.count = out;
        tinydb_generic_index_candidates_free(&source);
    }

    *candidates = working;
    set_message(message, message_size, "ok");
    return true;
}
