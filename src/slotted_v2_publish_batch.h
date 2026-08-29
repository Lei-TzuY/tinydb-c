#ifndef TINYDB_SLOTTED_V2_PUBLISH_BATCH_H
#define TINYDB_SLOTTED_V2_PUBLISH_BATCH_H

#include "pager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Recursive payload splits can touch both leaf siblings and several internal
 * ancestors in one publication boundary. Keep enough fixed, allocation-free
 * headroom for deeper cascades while preserving deterministic stack storage
 * and fail-closed capacity checks.
 */
#define TINYDB_V2_PUBLISH_BATCH_MAX_PAGES 16u
#define TINYDB_V2_PUBLISH_NO_FAIL UINT32_MAX

typedef struct {
    uint32_t page_num;
    unsigned char* target;
    const unsigned char* staged;
} TinyDBV2PublishEntry;

/*
 * Publish a validated collection of staged page images as one caller-visible
 * in-memory operation. All target pointers and page identities are validated
 * before the first byte is copied. If publication is interrupted at the
 * deterministic fail_after boundary, every target is restored byte-for-byte.
 *
 * Page zero is a valid TinyDB root page. INVALID_PAGE_NUM is the only reserved
 * page identity rejected here; callers that use zero as a topology sentinel
 * must enforce that invariant before constructing a publication batch.
 *
 * Only PAGE_USABLE_SIZE is copied. The final checksum trailer remains owned by
 * the Pager/WAL layer and is deliberately preserved.
 *
 * fail_after is a regression hook expressed as the number of completed page
 * copies after which publication should fail. Production callers pass
 * TINYDB_V2_PUBLISH_NO_FAIL.
 */
static bool tinydb_v2_publish_batch(
    const TinyDBV2PublishEntry* entries,
    uint32_t count,
    uint32_t fail_after) {
    if (entries == NULL || count == 0u ||
        count > TINYDB_V2_PUBLISH_BATCH_MAX_PAGES) {
        return false;
    }

    unsigned char before[TINYDB_V2_PUBLISH_BATCH_MAX_PAGES][PAGE_USABLE_SIZE];

    for (uint32_t i = 0u; i < count; i++) {
        if (entries[i].page_num == INVALID_PAGE_NUM ||
            entries[i].target == NULL || entries[i].staged == NULL) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (entries[j].page_num == entries[i].page_num ||
                entries[j].target == entries[i].target) {
                return false;
            }
        }
        memcpy(before[i], entries[i].target, PAGE_USABLE_SIZE);
    }

    if (fail_after == 0u) return false;

    for (uint32_t i = 0u; i < count; i++) {
        memcpy(entries[i].target, entries[i].staged, PAGE_USABLE_SIZE);
        if (fail_after != TINYDB_V2_PUBLISH_NO_FAIL && i + 1u == fail_after) {
            for (uint32_t j = 0u; j < count; j++) {
                memcpy(entries[j].target, before[j], PAGE_USABLE_SIZE);
            }
            return false;
        }
    }

    return true;
}

#endif
