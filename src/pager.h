#ifndef PAGER_H
#define PAGER_H

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define TABLE_MAX_PAGES   100
#define PAGE_SIZE         4096
#define PAGE_CHECKSUM_SIZE 4                          /* FNV-1a stored at page end */
#define PAGE_USABLE_SIZE  (PAGE_SIZE - PAGE_CHECKSUM_SIZE)

#define MAX_BUFFER_POOL_SIZE 16
#define INVALID_PAGE_NUM     0xFFFFFFFFu
#define MAX_SAVEPOINTS 8

typedef struct Frame {
    uint32_t page_num;        /* Page number loaded in this frame */
    void*    data;            /* 4096-byte memory buffer */
    bool     is_dirty;        /* Modified flag */
    uint32_t pin_count;       /* Active reference count */
    int      lru_prev;        /* Index of previous frame in LRU list */
    int      lru_next;        /* Index of next frame in LRU list */
} Frame;

typedef struct {
    char name[64];
    uint32_t num_pages;
    uint32_t free_page_count;
    uint32_t free_pages[TABLE_MAX_PAGES];
    bool is_dirty[TABLE_MAX_PAGES];
    void* page_snapshots[TABLE_MAX_PAGES];
} Savepoint;

typedef struct {
    FILE* file;
    char filename[512];
    char wal_filename[512];
    uint32_t file_length;
    uint32_t num_pages;
    bool in_transaction;
    uint32_t transaction_file_length;
    uint32_t transaction_num_pages;
    uint32_t free_pages[TABLE_MAX_PAGES]; /* stack of reusable page numbers */
    uint32_t free_page_count;
    uint32_t transaction_free_page_count; /* snapshot taken at BEGIN */
    
    /* Buffer Pool Manager */
    Frame    frames[MAX_BUFFER_POOL_SIZE];
    int      page_table[TABLE_MAX_PAGES]; /* maps page_num -> frame_index (-1 if evicted) */
    int      lru_head;                    /* Most Recently Used (MRU) frame index */
    int      lru_tail;                    /* Least Recently Used (LRU) frame index */
    
    /* Statistics */
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t evictions;

    bool is_dirty[TABLE_MAX_PAGES];
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

#endif // PAGER_H
