#include "pager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pager_checkpoint_legacy_base(Pager* pager);

static void allocation_failure(void) {
    fprintf(stderr, "Unable to snapshot dirty pages before checkpoint.\n");
    exit(EXIT_FAILURE);
}

void pager_checkpoint(Pager* pager) {
    if (pager == NULL) return;

    uint32_t dirty_count = 0u;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        if (pager->is_dirty[i]) dirty_count++;
    }
    if (dirty_count == 0u) {
        pager_checkpoint_legacy_base(pager);
        return;
    }

    uint32_t* dirty_pages =
        (uint32_t*)malloc((size_t)dirty_count * sizeof(uint32_t));
    unsigned char* dirty_images =
        (unsigned char*)malloc((size_t)dirty_count * PAGE_SIZE);
    if (dirty_pages == NULL || dirty_images == NULL) {
        free(dirty_pages);
        free(dirty_images);
        allocation_failure();
    }

    uint32_t saved = 0u;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        if (!pager->is_dirty[i]) continue;
        dirty_pages[saved] = i;
        memcpy(dirty_images + (size_t)saved * PAGE_SIZE,
               get_page(pager, i),
               PAGE_SIZE);
        pager_unpin_page(pager, i);

        /* Temporarily hide the newer non-transactional image so the legacy
         * checkpoint can first publish older WAL-committed shadows. */
        pager->is_dirty[i] = false;
        int frame_index = pager->page_table[i];
        if (frame_index != -1) pager->frames[frame_index].is_dirty = false;
        saved++;
    }

    pager_checkpoint_legacy_base(pager);

    /* Restore the images that happened after the committed shadows and flush
     * them in a second checkpoint pass. This makes temporal precedence
     * explicit: committed WAL image first, newer maintenance/direct dirty
     * image last. */
    for (uint32_t i = 0u; i < saved; i++) {
        uint32_t page_num = dirty_pages[i];
        void* page = get_page(pager, page_num);
        memcpy(page,
               dirty_images + (size_t)i * PAGE_SIZE,
               PAGE_SIZE);
        mark_page_dirty(pager, page_num);
        pager_unpin_page(pager, page_num);
    }

    free(dirty_images);
    free(dirty_pages);
    pager_checkpoint_legacy_base(pager);
}
