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

/* Multi-threaded Page RW Lock API */
void pager_acquire_read_lock(Pager* pager, uint32_t page_num);
void pager_release_read_lock(Pager* pager, uint32_t page_num);
void pager_acquire_write_lock(Pager* pager, uint32_t page_num);
void pager_release_write_lock(Pager* pager, uint32_t page_num);

#endif // PAGER_H
