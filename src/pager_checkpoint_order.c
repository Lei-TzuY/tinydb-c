#include "pager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pager_checkpoint_legacy_base(Pager* pager);

static void allocation_failure(void) {
    fprintf(stderr, "Unable to snapshot overlapping dirty pages before checkpoint.\n");
    exit(EXIT_FAILURE);
}

void pager_checkpoint(Pager* pager) {
    if (pager == NULL) return;

    /* Only pages that carry both an older committed shadow and a newer dirty
     * image need special ordering. Plain dirty pages can remain on the legacy
     * checkpoint fast path without extra copies or a second write. */
    uint32_t overlap_count = 0u;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        if (pager->is_dirty[i] && pager->committed_pages[i] != NULL) {
            overlap_count++;
        }
    }
    if (overlap_count == 0u) {
        pager_checkpoint_legacy_base(pager);
        return;
    }

    uint32_t* overlap_pages =
        (uint32_t*)malloc((size_t)overlap_count * sizeof(uint32_t));
    unsigned char* overlap_images =
        (unsigned char*)malloc((size_t)overlap_count * PAGE_SIZE);
    if (overlap_pages == NULL || overlap_images == NULL) {
        free(overlap_pages);
        free(overlap_images);
        allocation_failure();
    }

    uint32_t saved = 0u;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        if (!pager->is_dirty[i] || pager->committed_pages[i] == NULL) continue;

        overlap_pages[saved] = i;
        memcpy(overlap_images + (size_t)saved * PAGE_SIZE,
               get_page(pager, i),
               PAGE_SIZE);
        pager_unpin_page(pager, i);

        /* Temporarily hide only the newer overlapping image so the first pass
         * can publish its older WAL-committed shadow. Other dirty pages remain
         * visible and are flushed normally by that same pass. */
        pager->is_dirty[i] = false;
        int frame_index = pager->page_table[i];
        if (frame_index != -1) pager->frames[frame_index].is_dirty = false;
        saved++;
    }

    pager_checkpoint_legacy_base(pager);

    /* Restore the newer overlapping images after committed shadows have been
     * published, then flush only those restored pages in the second pass. */
    for (uint32_t i = 0u; i < saved; i++) {
        uint32_t page_num = overlap_pages[i];
        void* page = get_page(pager, page_num);
        memcpy(page,
               overlap_images + (size_t)i * PAGE_SIZE,
               PAGE_SIZE);
        mark_page_dirty(pager, page_num);
        pager_unpin_page(pager, page_num);
    }

    free(overlap_images);
    free(overlap_pages);
    pager_checkpoint_legacy_base(pager);
}
