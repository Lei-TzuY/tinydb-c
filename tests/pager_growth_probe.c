#include "pager.h"
#include "slotted_v2_pager_publish_batch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_PAGE (TABLE_MAX_PAGES + 32u)
#define MARKER_BASE 0xC0DE0000u
#define PUBLISH_MARKER_BASE 0xA7100000u
#define PREEXISTING_DIRTY_MARKER_BASE 0xD17A0000u
#define PREEXISTING_DIRTY_PAGE 7u
#define PUBLISH_PAGE_COUNT 64u
#define PUBLISH_FAIL_AFTER 33u
#define EVICTION_CHURN_START 128u
#define EVICTION_CHURN_PAGES (MAX_BUFFER_POOL_SIZE * 3u)

static int verify_page(Pager* pager, uint32_t page_num) {
    uint32_t marker = 0;
    void* page = get_page(pager, page_num);
    memcpy(&marker, page, sizeof(marker));
    return marker == (MARKER_BASE ^ page_num) ? 0 : 1;
}

static int verify_publish_marker(Pager* pager,
                                 uint32_t page_num,
                                 uint32_t marker_base) {
    uint32_t marker = 0;
    void* page = get_page(pager, page_num);
    memcpy(&marker, page, sizeof(marker));
    return marker == (marker_base ^ page_num) ? 0 : 1;
}

static void force_eviction_churn(Pager* pager) {
    for (uint32_t i = 0u; i < EVICTION_CHURN_PAGES; i++) {
        uint32_t page_num = EVICTION_CHURN_START + i;
        (void)get_page(pager, page_num);
        pager_unpin_page(pager, page_num);
    }
}

static int exercise_pager_aware_publication(Pager* pager) {
    unsigned char* staged = (unsigned char*)calloc(
        PUBLISH_PAGE_COUNT, PAGE_SIZE);
    TinyDBV2PagerPublishEntry* entries =
        (TinyDBV2PagerPublishEntry*)calloc(
            PUBLISH_PAGE_COUNT, sizeof(TinyDBV2PagerPublishEntry));
    if (staged == NULL || entries == NULL) {
        free(entries);
        free(staged);
        fprintf(stderr, "unable to allocate pager-aware publication probe\n");
        return 1;
    }

    /*
     * Start with one page already dirty before staged publication begins, then
     * force it out of the 16-frame cache. This proves rollback restores both
     * the pre-existing transactional image and its dirty state even when the
     * before-image originally came back through no-steal spill storage.
     */
    {
        unsigned char* page =
            (unsigned char*)get_page(pager, PREEXISTING_DIRTY_PAGE);
        uint32_t marker =
            PREEXISTING_DIRTY_MARKER_BASE ^ PREEXISTING_DIRTY_PAGE;
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, PREEXISTING_DIRTY_PAGE);
        pager_unpin_page(pager, PREEXISTING_DIRTY_PAGE);
    }

    uint32_t preexisting_evictions = pager->evictions;
    force_eviction_churn(pager);
    if (pager->evictions <= preexisting_evictions ||
        pager->page_table[PREEXISTING_DIRTY_PAGE] != -1 ||
        !pager_page_is_dirty(pager, PREEXISTING_DIRTY_PAGE) ||
        verify_publish_marker(pager,
                              PREEXISTING_DIRTY_PAGE,
                              PREEXISTING_DIRTY_MARKER_BASE) != 0) {
        fprintf(stderr,
                "pre-existing dirty page did not survive no-steal eviction\n");
        free(entries);
        free(staged);
        return 1;
    }

    for (uint32_t i = 0u; i < PUBLISH_PAGE_COUNT; i++) {
        unsigned char* staged_page = staged + (size_t)i * PAGE_SIZE;
        memcpy(staged_page, get_page(pager, i), PAGE_SIZE);
        uint32_t marker = PUBLISH_MARKER_BASE ^ i;
        memcpy(staged_page, &marker, sizeof(marker));
        entries[i].page_num = i;
        entries[i].staged = staged_page;
    }

    uint32_t evictions_before = pager->evictions;
    if (tinydb_v2_pager_publish_batch(pager,
                                      entries,
                                      PUBLISH_PAGE_COUNT,
                                      PUBLISH_FAIL_AFTER)) {
        fprintf(stderr, "injected pager-aware publication unexpectedly succeeded\n");
        free(entries);
        free(staged);
        return 1;
    }
    for (uint32_t i = 0u; i < PUBLISH_PAGE_COUNT; i++) {
        uint32_t expected_marker_base = i == PREEXISTING_DIRTY_PAGE
            ? PREEXISTING_DIRTY_MARKER_BASE
            : MARKER_BASE;
        bool expected_dirty = i == PREEXISTING_DIRTY_PAGE;
        if (verify_publish_marker(pager, i, expected_marker_base) != 0 ||
            pager_page_is_dirty(pager, i) != expected_dirty) {
            fprintf(stderr,
                    "pager-aware rollback failed on page %u\n",
                    i);
            free(entries);
            free(staged);
            return 1;
        }
    }
    if (pager->evictions <= evictions_before) {
        fprintf(stderr, "pager-aware publication did not exercise LRU eviction\n");
        free(entries);
        free(staged);
        return 1;
    }

    /* Re-evict the restored dirty page and prove its rolled-back bytes are now
     * the spill image, rather than the rejected staged publication image. */
    force_eviction_churn(pager);
    if (pager->page_table[PREEXISTING_DIRTY_PAGE] != -1 ||
        verify_publish_marker(pager,
                              PREEXISTING_DIRTY_PAGE,
                              PREEXISTING_DIRTY_MARKER_BASE) != 0 ||
        !pager_page_is_dirty(pager, PREEXISTING_DIRTY_PAGE)) {
        fprintf(stderr,
                "pre-existing dirty rollback did not survive re-eviction\n");
        free(entries);
        free(staged);
        return 1;
    }

    if (!tinydb_v2_pager_publish_batch(pager,
                                       entries,
                                       PUBLISH_PAGE_COUNT,
                                       TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "pager-aware publication failed\n");
        free(entries);
        free(staged);
        return 1;
    }
    for (uint32_t i = 0u; i < PUBLISH_PAGE_COUNT; i++) {
        if (verify_publish_marker(pager, i, PUBLISH_MARKER_BASE) != 0 ||
            !pager_page_is_dirty(pager, i)) {
            fprintf(stderr,
                    "pager-aware publication mismatch on page %u\n",
                    i);
            free(entries);
            free(staged);
            return 1;
        }
    }

    free(entries);
    free(staged);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: tinydb_pager_growth_probe DATABASE\n");
        return 2;
    }

    Pager* pager = pager_open(argv[1]);
    if (pager->num_pages != 0) {
        fprintf(stderr, "probe database must be empty\n");
        pager_close(pager);
        return 2;
    }

    for (uint32_t page_num = 0; page_num <= TARGET_PAGE; page_num++) {
        uint32_t marker = MARKER_BASE ^ page_num;
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, page_num);
    }

    if (pager_metadata_capacity(pager) <= TABLE_MAX_PAGES) {
        fprintf(stderr,
                "pager metadata did not grow beyond legacy ceiling: %u\n",
                pager_metadata_capacity(pager));
        pager_close(pager);
        return 1;
    }

    pager_commit(pager);
    pager_checkpoint(pager);
    uint32_t capacity_after_growth = pager_metadata_capacity(pager);
    uint32_t pages_after_growth = pager->num_pages;
    pager_close(pager);

    pager = pager_open(argv[1]);
    if (pager->num_pages != TARGET_PAGE + 1u) {
        fprintf(stderr,
                "unexpected page count after reopen: got=%u expected=%u\n",
                pager->num_pages,
                TARGET_PAGE + 1u);
        pager_close(pager);
        return 1;
    }

    const uint32_t samples[] = {
        0u,
        1u,
        TABLE_MAX_PAGES - 1u,
        TABLE_MAX_PAGES,
        TARGET_PAGE
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        if (verify_page(pager, samples[i]) != 0) {
            fprintf(stderr, "marker mismatch on page %u\n", samples[i]);
            pager_close(pager);
            return 1;
        }
    }

    if (exercise_pager_aware_publication(pager) != 0) {
        pager_close(pager);
        return 1;
    }

    printf("PAGER_GROWTH_OK pages=%u capacity=%u legacy_ceiling=%u "
           "pager_publish_pages=%u buffer_pool=%u eviction_safe=yes "
           "publish_rollback=yes preexisting_dirty_rollback=yes\n",
           pages_after_growth,
           capacity_after_growth,
           (unsigned)TABLE_MAX_PAGES,
           (unsigned)PUBLISH_PAGE_COUNT,
           (unsigned)MAX_BUFFER_POOL_SIZE);
    pager_close(pager);
    return 0;
}
