#ifndef TINYDB_SLOTTED_V2_PUBLISH_BATCH_H
#define TINYDB_SLOTTED_V2_PUBLISH_BATCH_H

#include "pager.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Recursive payload splits can touch both leaf siblings and several internal
 * ancestors in one publication boundary. Bound rollback memory by bytes rather
 * than by an arbitrary tree-height/page-count constant: callers can publish a
 * deeper cascade as long as the complete before-image fits this budget.
 *
 * The default is intentionally conservative and may be raised at build time
 * for workloads that need unusually deep atomic page publication. Rollback
 * images stay on the heap so increasing the budget does not enlarge stack
 * frames.
 */
#ifndef TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES
#define TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES (4u * 1024u * 1024u)
#endif

#if TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES < PAGE_USABLE_SIZE
#error "TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES must fit at least one page"
#endif

/* Compatibility/inspection value derived from the byte budget, not a tree cap. */
#define TINYDB_V2_PUBLISH_BATCH_MAX_PAGES \
    (TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES / PAGE_USABLE_SIZE)
#define TINYDB_V2_PUBLISH_NO_FAIL UINT32_MAX

typedef struct {
    uint32_t page_num;
    unsigned char* target;
    const unsigned char* staged;
} TinyDBV2PublishEntry;

/*
 * Validate the allocation shape before any entry is dereferenced. Keeping this
 * separate makes it possible for recursive callers and regression probes to
 * reject a malformed/over-budget count without fabricating an equally large
 * entry array first.
 */
static bool tinydb_v2_publish_batch_snapshot_size(
    uint32_t count,
    size_t* snapshot_bytes_out) {
    if (snapshot_bytes_out != NULL) *snapshot_bytes_out = 0u;
    if (count == 0u) return false;
#if SIZE_MAX < UINT32_MAX
    if (PAGE_USABLE_SIZE != 0u &&
        (size_t)count > SIZE_MAX / (size_t)PAGE_USABLE_SIZE) {
        return false;
    }
#endif

    size_t snapshot_bytes = (size_t)count * (size_t)PAGE_USABLE_SIZE;
    if (snapshot_bytes >
        (size_t)TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES) {
        return false;
    }
    if (snapshot_bytes_out != NULL) *snapshot_bytes_out = snapshot_bytes;
    return true;
}

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
    if (entries == NULL || fail_after == 0u) return false;

    size_t snapshot_bytes = 0u;
    if (!tinydb_v2_publish_batch_snapshot_size(count, &snapshot_bytes)) {
        return false;
    }

    /*
     * Finish every topology check before allocating rollback storage or
     * touching a caller page. This preserves the original all-or-nothing
     * preflight contract while moving the potentially large rollback image
     * out of the thread stack.
     */
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
    }

    unsigned char* before = (unsigned char*)malloc(snapshot_bytes);
    if (before == NULL) return false;

    for (uint32_t i = 0u; i < count; i++) {
        memcpy(before + (size_t)i * (size_t)PAGE_USABLE_SIZE,
               entries[i].target,
               PAGE_USABLE_SIZE);
    }

    for (uint32_t i = 0u; i < count; i++) {
        memcpy(entries[i].target, entries[i].staged, PAGE_USABLE_SIZE);
        if (fail_after != TINYDB_V2_PUBLISH_NO_FAIL && i + 1u == fail_after) {
            for (uint32_t j = 0u; j < count; j++) {
                memcpy(entries[j].target,
                       before + (size_t)j * (size_t)PAGE_USABLE_SIZE,
                       PAGE_USABLE_SIZE);
            }
            free(before);
            return false;
        }
    }

    free(before);
    return true;
}

#endif
