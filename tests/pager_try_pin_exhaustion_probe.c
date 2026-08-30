#include "pager_try_pin.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_COUNT (MAX_BUFFER_POOL_SIZE + 1u)
#define MARKER_BASE 0x7A110000u
#define EXTRA_PAGE MAX_BUFFER_POOL_SIZE

static uint32_t expected_marker(uint32_t page_num) {
    return MARKER_BASE ^ page_num;
}

static bool handle_marker_matches(PagerPageHandle* handle, uint32_t page_num) {
    uint32_t marker = 0u;
    if (handle == NULL || !handle->pinned || handle->data == NULL) return false;
    memcpy(&marker, handle->data, sizeof(marker));
    return marker == expected_marker(page_num);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: pager_try_pin_exhaustion_probe DATABASE\n");
        return 2;
    }

    Pager* pager = pager_open(argv[1]);
    if (pager->num_pages != 0u) {
        fprintf(stderr, "probe database must be empty\n");
        pager_close(pager);
        return 2;
    }

    for (uint32_t page_num = 0u; page_num < PAGE_COUNT; page_num++) {
        uint32_t marker = expected_marker(page_num);
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, page_num);
        pager_unpin_page(pager, page_num);
    }
    pager_checkpoint(pager);

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    for (uint32_t page_num = 0u; page_num < MAX_BUFFER_POOL_SIZE; page_num++) {
        if (!pager_pin_page_handle(pager, page_num, &owners[page_num]) ||
            !handle_marker_matches(&owners[page_num], page_num)) {
            fprintf(stderr, "unable to establish full-pool ownership at page %u\n",
                    page_num);
            return 1;
        }
    }

    PagerPageHandle extra_resident;
    if (pager_try_pin_existing_page_handle(pager, 1u, &extra_resident) !=
            PAGER_TRY_PIN_OK ||
        !handle_marker_matches(&extra_resident, 1u)) {
        fprintf(stderr, "resident try-pin failed while pool was fully pinned\n");
        return 1;
    }
    if (!pager_release_page_handle(&extra_resident)) {
        fprintf(stderr, "unable to release resident try-pin handle\n");
        return 1;
    }

    PagerPageHandle blocked;
    uint32_t evictions_before = pager->evictions;
    PagerTryPinStatus blocked_status =
        pager_try_pin_existing_page_handle(pager, EXTRA_PAGE, &blocked);
    if (blocked_status != PAGER_TRY_PIN_BUSY || blocked.pinned ||
        pager->evictions != evictions_before) {
        fprintf(stderr,
                "nonresident try-pin did not fail closed on all-pinned pool\n");
        return 1;
    }

    PagerPageHandle invalid;
    if (pager_try_pin_existing_page_handle(pager, PAGE_COUNT, &invalid) !=
            PAGER_TRY_PIN_INVALID_PAGE ||
        invalid.pinned) {
        fprintf(stderr, "out-of-range try-pin did not return INVALID_PAGE\n");
        return 1;
    }

    if (!pager_release_page_handle(&owners[0])) {
        fprintf(stderr, "unable to release one owner for retry\n");
        return 1;
    }

    PagerPageHandle retry;
    PagerTryPinStatus retry_status =
        pager_try_pin_existing_page_handle(pager, EXTRA_PAGE, &retry);
    if (retry_status != PAGER_TRY_PIN_OK ||
        !handle_marker_matches(&retry, EXTRA_PAGE) ||
        pager->evictions <= evictions_before) {
        fprintf(stderr, "try-pin did not recover after one frame became available\n");
        return 1;
    }
    if (!pager_release_page_handle(&retry)) {
        fprintf(stderr, "unable to release retry handle\n");
        return 1;
    }

    for (uint32_t page_num = 1u; page_num < MAX_BUFFER_POOL_SIZE; page_num++) {
        if (!pager_release_page_handle(&owners[page_num])) {
            fprintf(stderr, "unable to release owner %u\n", page_num);
            return 1;
        }
    }

    if (!pager_try_close(pager)) {
        fprintf(stderr, "try-pin exhaustion probe leaked ownership\n");
        return 1;
    }

    printf("PAGER_TRY_PIN_EXHAUSTION_OK all_pinned_busy=yes "
           "resident_hit=yes release_retry=yes invalid_page=yes "
           "no_process_exit=yes checksum_marker=yes\n");
    return 0;
}
