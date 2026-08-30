#ifndef PAGER_H
#define PAGER_H

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/*
 * TABLE_MAX_PAGES is retained as a legacy compatibility/sanity constant for
 * older auxiliary-file validation paths. The Pager itself no longer uses it
 * as a hard page-number ceiling.
 */
#ifndef TABLE_MAX_PAGES
#define TABLE_MAX_PAGES 4096u
#endif

#ifndef PAGER_INITIAL_CAPACITY
#define PAGER_INITIAL_CAPACITY 64u
#endif

#define PAGE_SIZE          4096u
#define PAGE_CHECKSUM_SIZE 4u
#define PAGE_USABLE_SIZE   (PAGE_SIZE - PAGE_CHECKSUM_SIZE)

#define MAX_BUFFER_POOL_SIZE 16
#define INVALID_PAGE_NUM     0xFFFFFFFFu
#define MAX_SAVEPOINTS       8

typedef struct Frame {
    uint32_t page_num;        /* Page number loaded in this frame */
    void*    data;            /* PAGE_SIZE-byte memory buffer */
    bool     is_dirty;        /* Modified flag */
    uint32_t pin_count;       /* Active reference count */
    int      lru_prev;        /* Index of previous frame in LRU list */
    int      lru_next;        /* Index of next frame in LRU list */
    db_rwlock_t rwlock;       /* Per-frame Read-Write lock */
} Frame;

typedef struct {
    char name[64];
    uint64_t file_length;
    uint32_t num_pages;
    uint32_t free_page_count;
    uint32_t capacity;
    uint32_t* free_pages;
    bool* is_dirty;
    void** page_snapshots;
} Savepoint;

typedef struct {
    FILE* file;
    char filename[512];
    char wal_filename[512];
    uint64_t file_length;
    uint32_t num_pages;
    uint32_t page_capacity;
    bool in_transaction;
    uint64_t transaction_file_length;
    uint32_t transaction_num_pages;

    uint32_t* free_pages; /* stack of reusable page numbers */
    uint32_t free_page_count;
    uint32_t transaction_free_page_count; /* snapshot taken at BEGIN */
    uint32_t* transaction_free_pages;

    /*
     * No-steal shadow storage.
     * dirty_page_spills keeps uncommitted pages that had to leave the small
     * buffer pool. committed_pages keeps the latest WAL-committed image until
     * checkpoint writes it to the main database file.
     */
    void** dirty_page_spills;
    void** committed_pages;

    /* Buffer Pool Manager */
    Frame frames[MAX_BUFFER_POOL_SIZE];
    int*  page_table; /* maps page_num -> frame_index (-1 if evicted) */
    int   lru_head;   /* Most Recently Used (MRU) frame index */
    int   lru_tail;   /* Least Recently Used (LRU) frame index */
    db_rwlock_t pager_lock; /* protects buffer-pool metadata */

    /* Statistics */
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t evictions;

    bool* is_dirty;
    Savepoint savepoints[MAX_SAVEPOINTS];
    uint32_t savepoint_count;
} Pager;

typedef enum {
    PAGER_PAGE_LOCK_NONE = 0,
    PAGER_PAGE_LOCK_READ,
    PAGER_PAGE_LOCK_WRITE
} PagerPageLockMode;

typedef struct {
    Pager* pager;
    uint32_t page_num;
    int frame_idx;
    void* data;
    bool pinned;
    PagerPageLockMode lock_mode;
} PagerPageHandle;

Pager* pager_open(const char* filename);
void pager_close(Pager* pager);
void* get_page(Pager* pager, uint32_t page_num);
void pager_unpin_page(Pager* pager, uint32_t page_num);
void pager_print_buffer_pool_stats(Pager* pager);
void pager_flush(Pager* pager, uint32_t page_num, uint32_t size);
uint32_t get_unused_page_num(Pager* pager);
uint32_t pager_metadata_capacity(Pager* pager);

void mark_page_dirty(Pager* pager, uint32_t page_num);
void pager_free_page(Pager* pager, uint32_t page_num);
void pager_shrink(Pager* pager, uint32_t new_num_pages);
void pager_begin_transaction(Pager* pager);
void pager_commit(Pager* pager);
void pager_rollback(Pager* pager);
void pager_checkpoint(Pager* pager);

bool pager_savepoint(Pager* pager, const char* name);
bool pager_rollback_to_savepoint(Pager* pager, const char* name);
bool pager_release_savepoint(Pager* pager, const char* name);

/*
 * Stable pinned-page handles for callers that retain a page pointer across
 * other LRU cache activity. get_page() alone returns an unpinned frame pointer;
 * the frame may be recycled by a later cache miss. This handle revalidates the
 * page/frame identity while holding pager_lock, then increments pin_count so
 * lru_evict() must skip the frame until the handle is released.
 */
static inline bool pager_pin_page_handle(Pager* pager,
                                         uint32_t page_num,
                                         PagerPageHandle* handle) {
    if (handle == NULL) return false;
    memset(handle, 0, sizeof(*handle));
    handle->page_num = INVALID_PAGE_NUM;
    handle->frame_idx = -1;
    if (pager == NULL || page_num == INVALID_PAGE_NUM) return false;

    for (uint32_t attempt = 0u; attempt < 64u; attempt++) {
        void* data = get_page(pager, page_num);

        db_rwlock_wrlock(&pager->pager_lock);
        int frame_idx = page_num < pager->page_capacity
            ? pager->page_table[page_num]
            : -1;
        if (frame_idx >= 0 && frame_idx < MAX_BUFFER_POOL_SIZE &&
            pager->frames[frame_idx].page_num == page_num &&
            pager->frames[frame_idx].data == data &&
            pager->frames[frame_idx].pin_count != UINT32_MAX) {
            pager->frames[frame_idx].pin_count++;
            handle->pager = pager;
            handle->page_num = page_num;
            handle->frame_idx = frame_idx;
            handle->data = data;
            handle->pinned = true;
            handle->lock_mode = PAGER_PAGE_LOCK_NONE;
            db_rwlock_wrunlock(&pager->pager_lock);
            return true;
        }
        db_rwlock_wrunlock(&pager->pager_lock);
    }
    return false;
}

static inline bool pager_release_page_handle(PagerPageHandle* handle) {
    if (handle == NULL || !handle->pinned || handle->pager == NULL ||
        handle->lock_mode != PAGER_PAGE_LOCK_NONE) {
        return false;
    }

    Pager* pager = handle->pager;
    bool released = false;
    db_rwlock_wrlock(&pager->pager_lock);
    if (handle->frame_idx >= 0 &&
        handle->frame_idx < MAX_BUFFER_POOL_SIZE &&
        pager->frames[handle->frame_idx].page_num == handle->page_num &&
        pager->frames[handle->frame_idx].data == handle->data &&
        pager->frames[handle->frame_idx].pin_count > 0u) {
        pager->frames[handle->frame_idx].pin_count--;
        released = true;
    }
    db_rwlock_wrunlock(&pager->pager_lock);

    if (released) {
        handle->pager = NULL;
        handle->page_num = INVALID_PAGE_NUM;
        handle->frame_idx = -1;
        handle->data = NULL;
        handle->pinned = false;
    }
    return released;
}

static inline bool pager_page_handle_acquire_read(PagerPageHandle* handle) {
    if (handle == NULL || !handle->pinned || handle->pager == NULL ||
        handle->lock_mode != PAGER_PAGE_LOCK_NONE ||
        handle->frame_idx < 0 || handle->frame_idx >= MAX_BUFFER_POOL_SIZE) {
        return false;
    }
    db_rwlock_rdlock(&handle->pager->frames[handle->frame_idx].rwlock);
    handle->lock_mode = PAGER_PAGE_LOCK_READ;
    return true;
}

static inline bool pager_page_handle_release_read(PagerPageHandle* handle) {
    if (handle == NULL || !handle->pinned || handle->pager == NULL ||
        handle->lock_mode != PAGER_PAGE_LOCK_READ ||
        handle->frame_idx < 0 || handle->frame_idx >= MAX_BUFFER_POOL_SIZE) {
        return false;
    }
    db_rwlock_rdunlock(&handle->pager->frames[handle->frame_idx].rwlock);
    handle->lock_mode = PAGER_PAGE_LOCK_NONE;
    return true;
}

static inline bool pager_page_handle_acquire_write(PagerPageHandle* handle) {
    if (handle == NULL || !handle->pinned || handle->pager == NULL ||
        handle->lock_mode != PAGER_PAGE_LOCK_NONE ||
        handle->frame_idx < 0 || handle->frame_idx >= MAX_BUFFER_POOL_SIZE) {
        return false;
    }
    db_rwlock_wrlock(&handle->pager->frames[handle->frame_idx].rwlock);
    handle->lock_mode = PAGER_PAGE_LOCK_WRITE;
    return true;
}

static inline bool pager_page_handle_release_write(PagerPageHandle* handle) {
    if (handle == NULL || !handle->pinned || handle->pager == NULL ||
        handle->lock_mode != PAGER_PAGE_LOCK_WRITE ||
        handle->frame_idx < 0 || handle->frame_idx >= MAX_BUFFER_POOL_SIZE) {
        return false;
    }
    db_rwlock_wrunlock(&handle->pager->frames[handle->frame_idx].rwlock);
    handle->lock_mode = PAGER_PAGE_LOCK_NONE;
    return true;
}

/*
 * Lightweight page-state API for staged mutation helpers.
 *
 * pager_restore_page_dirty_state() changes only Pager bookkeeping. Callers
 * must restore the corresponding page bytes first. A clean rollback clears an
 * obsolete no-steal spill so a later cache miss cannot resurrect the rejected
 * staged image. A dirty rollback deliberately preserves dirty status so the
 * restored pre-mutation image remains part of the surrounding transaction.
 */
static inline bool pager_page_is_dirty(Pager* pager, uint32_t page_num) {
    if (pager == NULL) return false;

    db_rwlock_rdlock(&pager->pager_lock);
    bool dirty = page_num < pager->page_capacity
        ? pager->is_dirty[page_num]
        : false;
    db_rwlock_rdunlock(&pager->pager_lock);
    return dirty;
}

static inline void pager_restore_page_dirty_state(Pager* pager,
                                                  uint32_t page_num,
                                                  bool was_dirty) {
    if (pager == NULL) return;

    db_rwlock_wrlock(&pager->pager_lock);
    if (page_num >= pager->page_capacity) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return;
    }
    pager->is_dirty[page_num] = was_dirty;
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) pager->frames[frame_idx].is_dirty = was_dirty;
    if (!was_dirty) {
        free(pager->dirty_page_spills[page_num]);
        pager->dirty_page_spills[page_num] = NULL;
    }
    db_rwlock_wrunlock(&pager->pager_lock);
}

/*
 * get_page() currently returns an unowned, unpinned frame pointer. Existing
 * callers still invoke pager_unpin_page() after those reads, but there is no
 * matching pin to release. Keep that source pattern harmless: explicit pins
 * belong exclusively to PagerPageHandle or the page-number lock seam below,
 * and only those owners may decrement pin_count. This also prevents a generic
 * cleanup call from stealing a live handle/lock pin on the same page.
 */
static inline void pager_unpin_page_unowned_compat(Pager* pager,
                                                   uint32_t page_num) {
    (void)pager;
    (void)page_num;
}

/* Helpers used by source-compatible destructive-operation guards. */
static inline bool pager_frame_for_page_is_pinned_locked(Pager* pager,
                                                         uint32_t page_num) {
    if (page_num >= pager->page_capacity) return false;
    int frame_idx = pager->page_table[page_num];
    return frame_idx >= 0 && frame_idx < MAX_BUFFER_POOL_SIZE &&
        pager->frames[frame_idx].page_num == page_num &&
        pager->frames[frame_idx].pin_count > 0u;
}

static inline bool pager_any_frame_pinned_locked(Pager* pager) {
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].page_num != INVALID_PAGE_NUM &&
            pager->frames[i].pin_count > 0u) {
            return true;
        }
    }
    return false;
}

static inline bool pager_removed_range_pinned_locked(Pager* pager,
                                                      uint32_t first_removed) {
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].page_num != INVALID_PAGE_NUM &&
            pager->frames[i].page_num >= first_removed &&
            pager->frames[i].pin_count > 0u) {
            return true;
        }
    }
    return false;
}

/*
 * Destructive Pager operations must not invalidate an explicitly pinned frame.
 * These three legacy operations do not recursively acquire pager_lock, so the
 * guard and mutation can execute under one write lock without a TOCTOU window.
 * Savepoint rollback has a different implementation shape (it re-enters
 * get_page/mark_page_dirty) and is intentionally not claimed here yet.
 */
static inline void pager_free_page_pin_guard(Pager* pager,
                                             uint32_t page_num) {
    if (pager == NULL) return;

    db_rwlock_wrlock(&pager->pager_lock);
    if (pager_frame_for_page_is_pinned_locked(pager, page_num)) {
        db_rwlock_wrunlock(&pager->pager_lock);
        fprintf(stderr, "Refusing to free pinned page %u.\n", page_num);
        return;
    }
    pager_free_page(pager, page_num);
    db_rwlock_wrunlock(&pager->pager_lock);
}

static inline void pager_shrink_pin_guard(Pager* pager,
                                          uint32_t new_num_pages) {
    if (pager == NULL) return;

    db_rwlock_wrlock(&pager->pager_lock);
    if (pager_removed_range_pinned_locked(pager, new_num_pages)) {
        db_rwlock_wrunlock(&pager->pager_lock);
        fprintf(stderr,
                "Refusing to shrink Pager across a pinned page boundary.\n");
        return;
    }
    pager_shrink(pager, new_num_pages);
    db_rwlock_wrunlock(&pager->pager_lock);
}

static inline void pager_rollback_pin_guard(Pager* pager) {
    if (pager == NULL) return;

    db_rwlock_wrlock(&pager->pager_lock);
    if (pager->in_transaction && pager_any_frame_pinned_locked(pager)) {
        db_rwlock_wrunlock(&pager->pager_lock);
        fprintf(stderr,
                "Refusing to rollback while Pager pages are pinned.\n");
        return;
    }
    pager_rollback(pager);
    db_rwlock_wrunlock(&pager->pager_lock);
}

/*
 * Source-compatible page-number lock seam. The historical implementation in
 * pager.c locked a resident frame without pinning it, so an unrelated cache
 * miss could recycle that frame while the caller still held the lock. Normal
 * translation units are redirected to these wrappers: they validate the
 * page/frame mapping and increment pin_count under pager_lock before waiting
 * for the per-frame lock. The pin remains held until the matching release, so
 * LRU eviction cannot invalidate the lock target.
 *
 * pager.c is compiled with a pager_checkpoint macro rename by CMake; that
 * implementation translation unit therefore keeps exporting the historical
 * ABI symbols without macro redirection, while in-tree/header consumers get
 * the eviction-safe compatibility path below.
 */
static inline bool pager_page_number_lock_pin(Pager* pager,
                                              uint32_t page_num,
                                              PagerPageLockMode lock_mode) {
    if (pager == NULL || page_num == INVALID_PAGE_NUM ||
        (lock_mode != PAGER_PAGE_LOCK_READ &&
         lock_mode != PAGER_PAGE_LOCK_WRITE)) {
        return false;
    }

    int frame_idx = -1;
    db_rwlock_wrlock(&pager->pager_lock);
    if (page_num < pager->page_capacity) {
        frame_idx = pager->page_table[page_num];
        if (frame_idx < 0 || frame_idx >= MAX_BUFFER_POOL_SIZE ||
            pager->frames[frame_idx].page_num != page_num ||
            pager->frames[frame_idx].pin_count == UINT32_MAX) {
            frame_idx = -1;
        } else {
            pager->frames[frame_idx].pin_count++;
        }
    }
    db_rwlock_wrunlock(&pager->pager_lock);

    if (frame_idx == -1) return false;
    if (lock_mode == PAGER_PAGE_LOCK_READ) {
        db_rwlock_rdlock(&pager->frames[frame_idx].rwlock);
    } else {
        db_rwlock_wrlock(&pager->frames[frame_idx].rwlock);
    }
    return true;
}

static inline bool pager_page_number_unlock_unpin(Pager* pager,
                                                  uint32_t page_num,
                                                  PagerPageLockMode lock_mode) {
    if (pager == NULL || page_num == INVALID_PAGE_NUM ||
        (lock_mode != PAGER_PAGE_LOCK_READ &&
         lock_mode != PAGER_PAGE_LOCK_WRITE)) {
        return false;
    }

    int frame_idx = -1;
    db_rwlock_rdlock(&pager->pager_lock);
    if (page_num < pager->page_capacity) {
        frame_idx = pager->page_table[page_num];
        if (frame_idx < 0 || frame_idx >= MAX_BUFFER_POOL_SIZE ||
            pager->frames[frame_idx].page_num != page_num ||
            pager->frames[frame_idx].pin_count == 0u) {
            frame_idx = -1;
        }
    }
    db_rwlock_rdunlock(&pager->pager_lock);
    if (frame_idx == -1) return false;

    if (lock_mode == PAGER_PAGE_LOCK_READ) {
        db_rwlock_rdunlock(&pager->frames[frame_idx].rwlock);
    } else {
        db_rwlock_wrunlock(&pager->frames[frame_idx].rwlock);
    }

    bool unpinned = false;
    db_rwlock_wrlock(&pager->pager_lock);
    if (page_num < pager->page_capacity &&
        pager->page_table[page_num] == frame_idx &&
        pager->frames[frame_idx].page_num == page_num &&
        pager->frames[frame_idx].pin_count > 0u) {
        pager->frames[frame_idx].pin_count--;
        unpinned = true;
    }
    db_rwlock_wrunlock(&pager->pager_lock);
    return unpinned;
}

static inline void pager_acquire_read_lock_pinned_compat(Pager* pager,
                                                         uint32_t page_num) {
    (void)pager_page_number_lock_pin(pager, page_num, PAGER_PAGE_LOCK_READ);
}

static inline void pager_release_read_lock_pinned_compat(Pager* pager,
                                                         uint32_t page_num) {
    (void)pager_page_number_unlock_unpin(pager, page_num,
                                         PAGER_PAGE_LOCK_READ);
}

static inline void pager_acquire_write_lock_pinned_compat(Pager* pager,
                                                          uint32_t page_num) {
    (void)pager_page_number_lock_pin(pager, page_num, PAGER_PAGE_LOCK_WRITE);
}

static inline void pager_release_write_lock_pinned_compat(Pager* pager,
                                                          uint32_t page_num) {
    (void)pager_page_number_unlock_unpin(pager, page_num,
                                         PAGER_PAGE_LOCK_WRITE);
}

/* Historical ABI declarations retained for pager.c and external symbol users. */
void pager_acquire_read_lock(Pager* pager, uint32_t page_num);
void pager_release_read_lock(Pager* pager, uint32_t page_num);
void pager_acquire_write_lock(Pager* pager, uint32_t page_num);
void pager_release_write_lock(Pager* pager, uint32_t page_num);

/*
 * In-tree consumers compile against the ownership-safe compatibility seams.
 * pager.c itself sees the CMake pager_checkpoint rename and is intentionally
 * excluded so its exported ABI definitions keep their original symbol names.
 */
#if !defined(pager_checkpoint)
#define pager_unpin_page         pager_unpin_page_unowned_compat
#define pager_free_page          pager_free_page_pin_guard
#define pager_shrink             pager_shrink_pin_guard
#define pager_rollback           pager_rollback_pin_guard
#define pager_acquire_read_lock  pager_acquire_read_lock_pinned_compat
#define pager_release_read_lock  pager_release_read_lock_pinned_compat
#define pager_acquire_write_lock pager_acquire_write_lock_pinned_compat
#define pager_release_write_lock pager_release_write_lock_pinned_compat
#endif

#endif // PAGER_H
