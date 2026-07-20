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
    void* pages[TABLE_MAX_PAGES];
    bool is_dirty[TABLE_MAX_PAGES];
} Pager;

Pager* pager_open(const char* filename);
void* get_page(Pager* pager, uint32_t page_num);
void pager_flush(Pager* pager, uint32_t page_num, uint32_t size);
uint32_t get_unused_page_num(Pager* pager);

void mark_page_dirty(Pager* pager, uint32_t page_num);
void pager_free_page(Pager* pager, uint32_t page_num);
void pager_shrink(Pager* pager, uint32_t new_num_pages);
void pager_begin_transaction(Pager* pager);
void pager_commit(Pager* pager);
void pager_rollback(Pager* pager);
void pager_checkpoint(Pager* pager);

#endif // PAGER_H
