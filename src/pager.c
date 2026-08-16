#include "pager.h"

#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define WAL_COMMIT_MAGIC 0x57414c43u /* "WALC" */

static void fail_io(const char* message) {
    printf("%s: %d\n", message, errno);
    exit(EXIT_FAILURE);
}

/* ── Page checksum (FNV-1a 32-bit) ──────────────────────────────────────
 * The last PAGE_CHECKSUM_SIZE bytes of every page store an FNV-1a hash of
 * the preceding PAGE_USABLE_SIZE bytes.  This lets us detect single-bit
 * flips, torn writes, and other forms of on-disk corruption at read time.
 */
static uint32_t fnv1a_32(const void* data, size_t len) {
    uint32_t hash = 2166136261u; /* FNV offset basis */
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 16777619u; /* FNV prime */
    }
    return hash;
}

static void page_write_checksum(void* page) {
    uint32_t csum = fnv1a_32(page, PAGE_USABLE_SIZE);
    memcpy((char*)page + PAGE_USABLE_SIZE, &csum, PAGE_CHECKSUM_SIZE);
}

static void page_verify_checksum(const void* page, uint32_t page_num) {
    uint32_t stored;
    memcpy(&stored, (const char*)page + PAGE_USABLE_SIZE, PAGE_CHECKSUM_SIZE);
    uint32_t computed = fnv1a_32(page, PAGE_USABLE_SIZE);
    if (stored != computed) {
        printf("Corruption detected: page %u checksum mismatch "
               "(stored 0x%08x, computed 0x%08x).\n",
               page_num, stored, computed);
        exit(EXIT_FAILURE);
    }
}

static void seek_file(FILE* file, uint32_t page_num) {
    if (fseek(file, (long)(page_num * PAGE_SIZE), SEEK_SET) != 0) {
        fail_io("Error seeking");
    }
}

static void write_bytes(FILE* file, const void* buffer, size_t size) {
    if (fwrite(buffer, 1, size, file) != size) {
        fail_io("Error writing");
    }
}

static bool read_bytes(FILE* file, void* buffer, size_t size) {
    return fread(buffer, 1, size, file) == size;
}

static void sync_file(FILE* file) {
    if (fflush(file) != 0) {
        fail_io("Error flushing");
    }

#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        fail_io("Error syncing");
    }
#else
    if (fsync(fileno(file)) != 0) {
        fail_io("Error syncing");
    }
#endif
}

static void truncate_file(FILE* file, uint32_t length) {
    if (fflush(file) != 0) {
        fail_io("Error flushing before truncate");
    }

#ifdef _WIN32
    if (_chsize_s(_fileno(file), length) != 0) {
        fail_io("Error truncating database");
    }
#else
    if (ftruncate(fileno(file), length) != 0) {
        fail_io("Error truncating database");
    }
#endif
}

static void lru_remove(Pager* pager, int frame_idx) {
    if (frame_idx == -1) return;
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

static void lru_touch(Pager* pager, int frame_idx) {
    if (pager->lru_head == frame_idx) return;

    lru_remove(pager, frame_idx);

    pager->frames[frame_idx].lru_next = pager->lru_head;
    pager->frames[frame_idx].lru_prev = -1;

    if (pager->lru_head != -1) {
        pager->frames[pager->lru_head].lru_prev = frame_idx;
    }
    pager->lru_head = frame_idx;

    if (pager->lru_tail == -1) {
        pager->lru_tail = frame_idx;
    }
}

static int lru_evict(Pager* pager) {
    int victim = pager->lru_tail;
    while (victim != -1) {
        if (pager->frames[victim].pin_count == 0) {
            break;
        }
        victim = pager->frames[victim].lru_prev;
    }

    if (victim == -1) {
        victim = pager->lru_tail;
    }

    uint32_t victim_page = pager->frames[victim].page_num;
    if (victim_page != INVALID_PAGE_NUM) {
        if (pager->frames[victim].is_dirty) {
            page_write_checksum(pager->frames[victim].data);
            seek_file(pager->file, victim_page);
            write_bytes(pager->file, pager->frames[victim].data, PAGE_SIZE);
            pager->frames[victim].is_dirty = false;
            uint32_t end_offset = victim_page * PAGE_SIZE + PAGE_SIZE;
            if (end_offset > pager->file_length) {
                pager->file_length = end_offset;
            }
        }
        pager->page_table[victim_page] = -1;
        pager->evictions++;
    }

    lru_remove(pager, victim);
    pager->frames[victim].page_num = INVALID_PAGE_NUM;
    pager->frames[victim].is_dirty = false;
    pager->frames[victim].pin_count = 0;

    return victim;
}

static void clear_page_cache(Pager* pager) {
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        pager->frames[i].page_num = INVALID_PAGE_NUM;
        pager->frames[i].is_dirty = false;
        pager->frames[i].pin_count = 0;
        pager->frames[i].lru_prev = -1;
        pager->frames[i].lru_next = -1;
    }
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->page_table[i] = -1;
        pager->is_dirty[i] = false;
    }
    pager->lru_head = -1;
    pager->lru_tail = -1;
}

static void recover_wal(const char* filename, const char* wal_filename) {
    FILE* wal_file = fopen(wal_filename, "rb");
    if (wal_file == NULL) {
        return;
    }

    printf("WAL file found. Recovering...\n");

    FILE* db_file = fopen(filename, "r+b");
    if (db_file == NULL) {
        db_file = fopen(filename, "w+b");
    }
    if (db_file == NULL) {
        fail_io("Unable to open database during recovery");
    }

    while (true) {
        uint32_t page_count;
        uint32_t page_nums[TABLE_MAX_PAGES];
        void* page_buffers[TABLE_MAX_PAGES];
        bool complete = true;

        for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
            page_buffers[i] = NULL;
        }

        size_t header_bytes = fread(&page_count, 1, sizeof(page_count), wal_file);
        if (header_bytes == 0) {
            break;
        }
        if (header_bytes != sizeof(page_count) ||
            page_count == 0 ||
            page_count > TABLE_MAX_PAGES) {
            printf("Ignoring incomplete WAL transaction.\n");
            break;
        }

        for (uint32_t i = 0; i < page_count; i++) {
            page_buffers[i] = malloc(PAGE_SIZE);
            if (page_buffers[i] == NULL) {
                printf("Unable to allocate WAL transaction buffer.\n");
                exit(EXIT_FAILURE);
            }

            if (!read_bytes(wal_file, &page_nums[i], sizeof(page_nums[i])) ||
                page_nums[i] >= TABLE_MAX_PAGES ||
                !read_bytes(wal_file, page_buffers[i], PAGE_SIZE)) {
                complete = false;
                break;
            }
        }

        uint32_t commit_magic = 0;
        if (complete && !read_bytes(wal_file, &commit_magic, sizeof(commit_magic))) {
            complete = false;
        }

        if (!complete || commit_magic != WAL_COMMIT_MAGIC) {
            printf("Ignoring incomplete WAL transaction.\n");
            for (uint32_t i = 0; i < page_count; i++) {
                free(page_buffers[i]);
            }
            break;
        }

        for (uint32_t i = 0; i < page_count; i++) {
            seek_file(db_file, page_nums[i]);
            write_bytes(db_file, page_buffers[i], PAGE_SIZE);
            free(page_buffers[i]);
        }
    }

    sync_file(db_file);
    fclose(db_file);
    fclose(wal_file);

    if (remove(wal_filename) != 0) {
        fail_io("Unable to remove recovered WAL");
    }
    printf("Recovery complete.\n");
}

Pager* pager_open(const char* filename) {
    Pager* pager = malloc(sizeof(Pager));
    if (pager == NULL) {
        printf("Unable to allocate pager.\n");
        exit(EXIT_FAILURE);
    }

    snprintf(pager->filename, sizeof(pager->filename), "%s", filename);
    snprintf(pager->wal_filename, sizeof(pager->wal_filename), "%s.wal", filename);
    recover_wal(pager->filename, pager->wal_filename);

    FILE* file = fopen(filename, "r+b");
    if (file == NULL) {
        file = fopen(filename, "w+b");
    }
    if (file == NULL) {
        fail_io("Unable to open database");
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fail_io("Error seeking");
    }

    long file_length = ftell(file);
    if (file_length < 0) {
        fail_io("Error reading database length");
    }
    if ((uint32_t)file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    pager->file = file;
    pager->file_length = (uint32_t)file_length;
    pager->num_pages = pager->file_length / PAGE_SIZE;
    pager->in_transaction = false;
    pager->transaction_file_length = 0;
    pager->transaction_num_pages = 0;
    pager->free_page_count = 0;
    pager->transaction_free_page_count = 0;
    pager->savepoint_count = 0;
    memset(pager->savepoints, 0, sizeof(pager->savepoints));

    pager->lru_head = -1;
    pager->lru_tail = -1;
    pager->cache_hits = 0;
    pager->cache_misses = 0;
    pager->evictions = 0;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->page_table[i] = -1;
        pager->is_dirty[i] = false;
    }

    db_rwlock_init(&pager->pager_lock);

    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        pager->frames[i].page_num = INVALID_PAGE_NUM;
        pager->frames[i].data = calloc(1, PAGE_SIZE);
        pager->frames[i].is_dirty = false;
        pager->frames[i].pin_count = 0;
        pager->frames[i].lru_prev = -1;
        pager->frames[i].lru_next = -1;
        db_rwlock_init(&pager->frames[i].rwlock);
    }

    return pager;
}

void* get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %u >= %u\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    db_rwlock_wrlock(&pager->pager_lock);

    int frame_idx = pager->page_table[page_num];

    if (frame_idx != -1) {
        pager->cache_hits++;
        lru_touch(pager, frame_idx);
        void* data = pager->frames[frame_idx].data;
        db_rwlock_wrunlock(&pager->pager_lock);
        return data;
    }

    pager->cache_misses++;

    /* Find empty frame */
    frame_idx = -1;
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].page_num == INVALID_PAGE_NUM) {
            frame_idx = i;
            break;
        }
    }

    if (frame_idx == -1) {
        frame_idx = lru_evict(pager);
    }

    void* page_data = pager->frames[frame_idx].data;
    memset(page_data, 0, PAGE_SIZE);

    uint32_t file_pages = pager->file_length / PAGE_SIZE;
    if (page_num < file_pages) {
        seek_file(pager->file, page_num);
        if (fread(page_data, 1, PAGE_SIZE, pager->file) != PAGE_SIZE) {
            fail_io("Error reading page");
        }
        page_verify_checksum(page_data, page_num);
    }

    if (page_num >= pager->num_pages) {
        pager->num_pages = page_num + 1;
    }

    pager->frames[frame_idx].page_num = page_num;
    pager->frames[frame_idx].is_dirty = pager->is_dirty[page_num];
    pager->frames[frame_idx].pin_count = 0;
    pager->page_table[page_num] = frame_idx;

    lru_touch(pager, frame_idx);

    db_rwlock_wrunlock(&pager->pager_lock);
    return page_data;
}

void pager_unpin_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) return;
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1 && pager->frames[frame_idx].pin_count > 0) {
        pager->frames[frame_idx].pin_count--;
    }
}

void pager_print_buffer_pool_stats(Pager* pager) {
    uint32_t total = pager->cache_hits + pager->cache_misses;
    double hit_ratio = (total > 0) ? ((double)pager->cache_hits / total * 100.0) : 0.0;

    printf("=== Buffer Pool Manager Statistics ===\n");
    printf("Capacity    : %d pages\n", MAX_BUFFER_POOL_SIZE);
    printf("Hits        : %u\n", pager->cache_hits);
    printf("Misses      : %u\n", pager->cache_misses);
    printf("Hit Ratio   : %.2f%%\n", hit_ratio);
    printf("Evictions   : %u\n", pager->evictions);
    printf("LRU Queue   (MRU -> LRU):\n");

    int curr = pager->lru_head;
    int pos = 0;
    while (curr != -1) {
        Frame* f = &pager->frames[curr];
        printf("  [%2d] Frame %2d -> Page %2u (dirty=%d, pins=%u)\n",
               pos++, curr, f->page_num, f->is_dirty ? 1 : 0, f->pin_count);
        curr = f->lru_next;
    }
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t size) {
    int frame_idx = pager->page_table[page_num];
    void* page_data = NULL;

    if (frame_idx != -1) {
        page_data = pager->frames[frame_idx].data;
    } else {
        return;
    }

    page_write_checksum(page_data);
    seek_file(pager->file, page_num);
    write_bytes(pager->file, page_data, size);

    uint32_t end_offset = page_num * PAGE_SIZE + size;
    if (end_offset > pager->file_length) {
        pager->file_length = end_offset;
    }
}

uint32_t get_unused_page_num(Pager* pager) {
    if (pager->free_page_count > 0) {
        return pager->free_pages[--pager->free_page_count];
    }
    return pager->num_pages;
}

void pager_free_page(Pager* pager, uint32_t page_num) {
    if (page_num == 0) {
        printf("BUG: attempted to free page 0 (always root).\n");
        exit(EXIT_FAILURE);
    }
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        pager->frames[frame_idx].page_num = INVALID_PAGE_NUM;
        pager->frames[frame_idx].is_dirty = false;
        pager->frames[frame_idx].pin_count = 0;
        lru_remove(pager, frame_idx);
        pager->page_table[page_num] = -1;
    }
    pager->is_dirty[page_num] = false;
    pager->free_pages[pager->free_page_count++] = page_num;
}

void pager_shrink(Pager* pager, uint32_t new_num_pages) {
    if (new_num_pages >= pager->num_pages) return;

    for (uint32_t i = new_num_pages; i < pager->num_pages; i++) {
        int frame_idx = pager->page_table[i];
        if (frame_idx != -1) {
            pager->frames[frame_idx].page_num = INVALID_PAGE_NUM;
            pager->frames[frame_idx].is_dirty = false;
            pager->frames[frame_idx].pin_count = 0;
            lru_remove(pager, frame_idx);
            pager->page_table[i] = -1;
        }
        pager->is_dirty[i] = false;
    }

    /* Remove free list entries that fall inside the dropped range. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] < new_num_pages) {
            pager->free_pages[kept++] = pager->free_pages[i];
        }
    }
    pager->free_page_count = kept;

    pager->num_pages = new_num_pages;
    pager->file_length = new_num_pages * PAGE_SIZE;
    truncate_file(pager->file, pager->file_length);
    sync_file(pager->file);
}

void mark_page_dirty(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to dirty an out-of-bounds page.\n");
        exit(EXIT_FAILURE);
    }
    pager->is_dirty[page_num] = true;
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        pager->frames[frame_idx].is_dirty = true;
    }
}

static void free_savepoint_snapshots(Savepoint* sp) {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (sp->page_snapshots[i] != NULL) {
            free(sp->page_snapshots[i]);
            sp->page_snapshots[i] = NULL;
        }
    }
}

static void clear_all_savepoints(Pager* pager) {
    for (uint32_t i = 0; i < pager->savepoint_count; i++) {
        free_savepoint_snapshots(&pager->savepoints[i]);
    }
    pager->savepoint_count = 0;
}

void pager_begin_transaction(Pager* pager) {
    if (pager->in_transaction) {
        printf("Pager transaction already active.\n");
        exit(EXIT_FAILURE);
    }

    /* Make every previous autocommit durable before a rollback point is taken. */
    pager_checkpoint(pager);
    clear_all_savepoints(pager);
    pager->transaction_file_length = pager->file_length;
    pager->transaction_num_pages = pager->num_pages;
    pager->transaction_free_page_count = pager->free_page_count;
    pager->in_transaction = true;
}

void pager_commit(Pager* pager) {
    bool has_dirty = false;
    uint32_t dirty_count = 0;

    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->is_dirty[i]) {
            has_dirty = true;
            dirty_count++;
        }
    }
    if (!has_dirty) {
        clear_all_savepoints(pager);
        pager->in_transaction = false;
        pager->transaction_file_length = 0;
        pager->transaction_num_pages = 0;
        return;
    }

    FILE* wal_file = fopen(pager->wal_filename, "ab");
    if (wal_file == NULL) {
        fail_io("Failed to open WAL for commit");
    }

    write_bytes(wal_file, &dirty_count, sizeof(dirty_count));
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->is_dirty[i]) {
            void* page_data = get_page(pager, i);
            page_write_checksum(page_data);
            write_bytes(wal_file, &i, sizeof(i));
            write_bytes(wal_file, page_data, PAGE_SIZE);
            pager_unpin_page(pager, i);
        }
    }
    uint32_t commit_magic = WAL_COMMIT_MAGIC;
    write_bytes(wal_file, &commit_magic, sizeof(commit_magic));

    sync_file(wal_file);
    fclose(wal_file);

    for (uint32_t i = 0; i < pager->num_pages; i++) {
        pager->is_dirty[i] = false;
    }
    clear_all_savepoints(pager);
    pager->in_transaction = false;
    pager->transaction_file_length = 0;
    pager->transaction_num_pages = 0;
}

void pager_rollback(Pager* pager) {
    if (!pager->in_transaction) {
        return;
    }

    clear_all_savepoints(pager);
    clear_page_cache(pager);
    pager->file_length = pager->transaction_file_length;
    pager->num_pages = pager->transaction_num_pages;
    pager->free_page_count = pager->transaction_free_page_count;
    truncate_file(pager->file, pager->transaction_file_length);
    sync_file(pager->file);
    pager->transaction_file_length = 0;
    pager->transaction_num_pages = 0;
    pager->transaction_free_page_count = 0;
    pager->in_transaction = false;
}

void pager_checkpoint(Pager* pager) {
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        void* page_data = get_page(pager, i);
        (void)page_data;
        pager_flush(pager, i, PAGE_SIZE);
        pager_unpin_page(pager, i);
    }
    sync_file(pager->file);

    if (remove(pager->wal_filename) != 0 && errno != ENOENT) {
        fail_io("Unable to remove checkpointed WAL");
    }
}

void pager_close(Pager* pager) {
    if (pager == NULL) return;
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].data != NULL) {
            free(pager->frames[i].data);
            pager->frames[i].data = NULL;
        }
        db_rwlock_destroy(&pager->frames[i].rwlock);
    }
    db_rwlock_destroy(&pager->pager_lock);
    fclose(pager->file);
    free(pager);
}

bool pager_savepoint(Pager* pager, const char* name) {
    if (pager->savepoint_count >= MAX_SAVEPOINTS) {
        return false;
    }
    Savepoint* sp = &pager->savepoints[pager->savepoint_count++];
    memset(sp, 0, sizeof(*sp));
    strncpy(sp->name, name, sizeof(sp->name) - 1);
    sp->num_pages = pager->num_pages;
    sp->free_page_count = pager->free_page_count;
    memcpy(sp->free_pages, pager->free_pages, sizeof(pager->free_pages));
    memcpy(sp->is_dirty, pager->is_dirty, sizeof(pager->is_dirty));
    
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        void* page_data = get_page(pager, i);
        sp->page_snapshots[i] = malloc(PAGE_SIZE);
        memcpy(sp->page_snapshots[i], page_data, PAGE_SIZE);
        pager_unpin_page(pager, i);
    }
    return true;
}

bool pager_rollback_to_savepoint(Pager* pager, const char* name) {
    int target_idx = -1;
    for (int i = (int)pager->savepoint_count - 1; i >= 0; i--) {
        if (strcmp(pager->savepoints[i].name, name) == 0) {
            target_idx = i;
            break;
        }
    }
    if (target_idx == -1) return false;

    Savepoint* sp = &pager->savepoints[target_idx];

    pager->num_pages = sp->num_pages;
    pager->free_page_count = sp->free_page_count;
    memcpy(pager->free_pages, sp->free_pages, sizeof(sp->free_pages));

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->is_dirty[i] = sp->is_dirty[i];
        if (sp->page_snapshots[i] != NULL) {
            void* page_data = get_page(pager, i);
            memcpy(page_data, sp->page_snapshots[i], PAGE_SIZE);
            pager_unpin_page(pager, i);
        }
    }

    for (uint32_t i = target_idx + 1; i < pager->savepoint_count; i++) {
        free_savepoint_snapshots(&pager->savepoints[i]);
    }
    pager->savepoint_count = target_idx + 1;
    return true;
}

bool pager_release_savepoint(Pager* pager, const char* name) {
    int target_idx = -1;
    for (int i = (int)pager->savepoint_count - 1; i >= 0; i--) {
        if (strcmp(pager->savepoints[i].name, name) == 0) {
            target_idx = i;
            break;
        }
    }
    if (target_idx == -1) return false;

    for (uint32_t i = target_idx; i < pager->savepoint_count; i++) {
        free_savepoint_snapshots(&pager->savepoints[i]);
    }
    pager->savepoint_count = target_idx;
    return true;
}

void pager_acquire_read_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        db_rwlock_rdlock(&pager->frames[frame_idx].rwlock);
    }
    db_rwlock_rdunlock(&pager->pager_lock);
}

void pager_release_read_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        db_rwlock_rdunlock(&pager->frames[frame_idx].rwlock);
    }
    db_rwlock_rdunlock(&pager->pager_lock);
}

void pager_acquire_write_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        db_rwlock_wrlock(&pager->frames[frame_idx].rwlock);
    }
    db_rwlock_rdunlock(&pager->pager_lock);
}

void pager_release_write_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        db_rwlock_wrunlock(&pager->frames[frame_idx].rwlock);
    }
    db_rwlock_rdunlock(&pager->pager_lock);
}
