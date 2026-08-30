#ifndef PAGER_TRY_PIN_H
#define PAGER_TRY_PIN_H

#include "pager.h"

/*
 * Non-fatal page acquisition for existing pages.
 *
 * Ordinary get_page() intentionally preserves the historical fail-fast
 * behavior when every buffer-pool frame is pinned. This API is a separate
 * source-level seam for callers that need backpressure instead: frame lookup,
 * replacement selection, dirty no-steal spill, page-image load, LRU mutation,
 * and publication of pin_count=1 all happen while pager_lock is held, so an
 * all-pinned pool returns PAGER_TRY_PIN_BUSY without a check/use race.
 *
 * The initial contract is deliberately limited to page_num < num_pages. It
 * does not allocate/grow Pager metadata or create new logical pages.
 */
typedef enum {
    PAGER_TRY_PIN_OK = 0,
    PAGER_TRY_PIN_BUSY,
    PAGER_TRY_PIN_INVALID_PAGE,
    PAGER_TRY_PIN_IO_ERROR,
    PAGER_TRY_PIN_CORRUPT_PAGE,
    PAGER_TRY_PIN_NO_MEMORY
} PagerTryPinStatus;

static inline uint32_t pager_try_pin_fnv1a32(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0u; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline bool pager_try_pin_checksum_valid(const void* page) {
    uint32_t stored = 0u;
    memcpy(&stored,
           (const uint8_t*)page + PAGE_USABLE_SIZE,
           PAGE_CHECKSUM_SIZE);
    return stored == pager_try_pin_fnv1a32(page, PAGE_USABLE_SIZE);
}

static inline bool pager_try_pin_seek_page(FILE* file, uint32_t page_num) {
    uint64_t offset = (uint64_t)page_num * (uint64_t)PAGE_SIZE;
    if (offset > (uint64_t)INT64_MAX) return false;
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static inline void pager_try_pin_lru_remove_locked(Pager* pager,
                                                    int frame_idx) {
    if (frame_idx < 0 || frame_idx >= MAX_BUFFER_POOL_SIZE) return;
    Frame* frame = &pager->frames[frame_idx];

    if (frame->lru_prev != -1) {
        pager->frames[frame->lru_prev].lru_next = frame->lru_next;
    } else if (pager->lru_head == frame_idx) {
        pager->lru_head = frame->lru_next;
    }

    if (frame->lru_next != -1) {
        pager->frames[frame->lru_next].lru_prev = frame->lru_prev;
    } else if (pager->lru_tail == frame_idx) {
        pager->lru_tail = frame->lru_prev;
    }

    frame->lru_prev = -1;
    frame->lru_next = -1;
}

static inline void pager_try_pin_lru_touch_locked(Pager* pager,
                                                   int frame_idx) {
    if (pager->lru_head == frame_idx) return;
    pager_try_pin_lru_remove_locked(pager, frame_idx);
    pager->frames[frame_idx].lru_next = pager->lru_head;
    pager->frames[frame_idx].lru_prev = -1;
    if (pager->lru_head != -1) {
        pager->frames[pager->lru_head].lru_prev = frame_idx;
    }
    pager->lru_head = frame_idx;
    if (pager->lru_tail == -1) pager->lru_tail = frame_idx;
}

static inline int pager_try_pin_choose_frame_locked(Pager* pager) {
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].page_num == INVALID_PAGE_NUM) return i;
    }

    int victim = pager->lru_tail;
    while (victim != -1 && pager->frames[victim].pin_count != 0u) {
        victim = pager->frames[victim].lru_prev;
    }
    return victim;
}

static inline void pager_try_pin_fill_handle(PagerPageHandle* handle,
                                              Pager* pager,
                                              uint32_t page_num,
                                              int frame_idx) {
    handle->pager = pager;
    handle->page_num = page_num;
    handle->frame_idx = frame_idx;
    handle->data = pager->frames[frame_idx].data;
    handle->pinned = true;
    handle->lock_mode = PAGER_PAGE_LOCK_NONE;
}

static inline PagerTryPinStatus pager_try_pin_existing_page_handle(
    Pager* pager,
    uint32_t page_num,
    PagerPageHandle* handle) {
    if (handle == NULL) return PAGER_TRY_PIN_INVALID_PAGE;
    memset(handle, 0, sizeof(*handle));
    handle->page_num = INVALID_PAGE_NUM;
    handle->frame_idx = -1;

    if (pager == NULL || page_num == INVALID_PAGE_NUM) {
        return PAGER_TRY_PIN_INVALID_PAGE;
    }

    db_rwlock_wrlock(&pager->pager_lock);

    if (pager->pin_barrier_active) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_BUSY;
    }
    if (page_num >= pager->num_pages || page_num >= pager->page_capacity) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_INVALID_PAGE;
    }

    int frame_idx = pager->page_table[page_num];
    if (frame_idx >= 0 && frame_idx < MAX_BUFFER_POOL_SIZE &&
        pager->frames[frame_idx].page_num == page_num) {
        if (pager->frames[frame_idx].pin_count == UINT32_MAX) {
            db_rwlock_wrunlock(&pager->pager_lock);
            return PAGER_TRY_PIN_BUSY;
        }
        pager->cache_hits++;
        pager->frames[frame_idx].pin_count++;
        pager_try_pin_lru_touch_locked(pager, frame_idx);
        pager_try_pin_fill_handle(handle, pager, page_num, frame_idx);
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_OK;
    }

    pager->cache_misses++;
    frame_idx = pager_try_pin_choose_frame_locked(pager);
    if (frame_idx == -1) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_BUSY;
    }

    uint8_t incoming[PAGE_SIZE];
    memset(incoming, 0, sizeof(incoming));
    if (pager->is_dirty[page_num] &&
        pager->dirty_page_spills[page_num] != NULL) {
        memcpy(incoming, pager->dirty_page_spills[page_num], PAGE_SIZE);
    } else if (pager->committed_pages[page_num] != NULL) {
        memcpy(incoming, pager->committed_pages[page_num], PAGE_SIZE);
    } else {
        uint64_t page_end = ((uint64_t)page_num + 1u) * (uint64_t)PAGE_SIZE;
        if (page_end <= pager->file_length) {
            if (!pager_try_pin_seek_page(pager->file, page_num)) {
                clearerr(pager->file);
                db_rwlock_wrunlock(&pager->pager_lock);
                return PAGER_TRY_PIN_IO_ERROR;
            }
            if (fread(incoming, 1u, PAGE_SIZE, pager->file) != PAGE_SIZE) {
                clearerr(pager->file);
                db_rwlock_wrunlock(&pager->pager_lock);
                return PAGER_TRY_PIN_IO_ERROR;
            }
            if (!pager_try_pin_checksum_valid(incoming)) {
                db_rwlock_wrunlock(&pager->pager_lock);
                return PAGER_TRY_PIN_CORRUPT_PAGE;
            }
        }
    }

    Frame* victim = &pager->frames[frame_idx];
    uint32_t victim_page = victim->page_num;
    void* new_spill = NULL;
    if (victim_page != INVALID_PAGE_NUM && victim->is_dirty &&
        pager->dirty_page_spills[victim_page] == NULL) {
        new_spill = malloc(PAGE_SIZE);
        if (new_spill == NULL) {
            db_rwlock_wrunlock(&pager->pager_lock);
            return PAGER_TRY_PIN_NO_MEMORY;
        }
    }

    if (victim_page != INVALID_PAGE_NUM) {
        if (victim->is_dirty) {
            if (new_spill != NULL) {
                pager->dirty_page_spills[victim_page] = new_spill;
                new_spill = NULL;
            }
            memcpy(pager->dirty_page_spills[victim_page],
                   victim->data,
                   PAGE_SIZE);
        }
        if (victim_page < pager->page_capacity &&
            pager->page_table[victim_page] == frame_idx) {
            pager->page_table[victim_page] = -1;
        }
        pager->evictions++;
    }
    free(new_spill);

    pager_try_pin_lru_remove_locked(pager, frame_idx);
    memcpy(victim->data, incoming, PAGE_SIZE);
    victim->page_num = page_num;
    victim->is_dirty = pager->is_dirty[page_num];
    victim->pin_count = 1u;
    pager->page_table[page_num] = frame_idx;
    pager_try_pin_lru_touch_locked(pager, frame_idx);
    pager_try_pin_fill_handle(handle, pager, page_num, frame_idx);

    db_rwlock_wrunlock(&pager->pager_lock);
    return PAGER_TRY_PIN_OK;
}

#endif /* PAGER_TRY_PIN_H */
