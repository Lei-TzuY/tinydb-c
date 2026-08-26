#include "pager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TARGET_PAGE (TABLE_MAX_PAGES + 32u)
#define MARKER_BASE 0xC0DE0000u

static int verify_page(Pager* pager, uint32_t page_num) {
    uint32_t marker = 0;
    void* page = get_page(pager, page_num);
    memcpy(&marker, page, sizeof(marker));
    return marker == (MARKER_BASE ^ page_num) ? 0 : 1;
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

    printf("PAGER_GROWTH_OK pages=%u capacity=%u legacy_ceiling=%u\n",
           pages_after_growth,
           capacity_after_growth,
           (unsigned)TABLE_MAX_PAGES);
    pager_close(pager);
    return 0;
}
