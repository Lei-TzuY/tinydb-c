#ifndef TINYDB_COMPACT_V2_MIGRATION_OPEN_ADAPTER_H
#define TINYDB_COMPACT_V2_MIGRATION_OPEN_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "compact_v2_migration_catalog_state.h"
#include "compact_v2_migration_manifest_file.h"
#include "compact_v2_migration_pager_recovery.h"

/*
 * Production file/catalog adapter used by tinydb_open().  Recovery needs one
 * context for both authoritative catalog lookup and durable sidecar cleanup.
 * Keeping those operations together prevents an open path from accidentally
 * consulting one Catalog while deleting a manifest belonging to another DB.
 *
 * Reclaim is additionally guarded by the complete authoritative multi-table
 * catalog.  A malformed or stale migration sidecar must never be able to free
 * the current root of another table merely because that page number appears in
 * its staging claims or retired-root field.
 */
typedef struct TinyDBCompactV2MigrationOpenAdapterContext {
    TinyDBCompactV2MigrationCatalogState catalog_state;
    char database_filename[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    char manifest_path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
#ifdef _WIN32
    char tombstone_path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
#endif
} TinyDBCompactV2MigrationOpenAdapterContext;

static inline bool tinydb_compact_v2_migration_open_adapter_init(
    TinyDBCompactV2MigrationOpenAdapterContext* context,
    const char* database_filename,
    const Catalog* catalog,
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot,
    Pager* pager) {
    size_t filename_length;

    if (context == NULL) return false;
    memset(context, 0, sizeof(*context));
    if (database_filename == NULL || database_filename[0] == '\0' ||
        catalog == NULL || snapshot == NULL || pager == NULL) {
        return false;
    }
    filename_length = strlen(database_filename);
    if (filename_length + 1u > sizeof(context->database_filename)) return false;
    memcpy(context->database_filename, database_filename, filename_length + 1u);
    if (!tinydb_compact_v2_migration_manifest_file_path(
            database_filename,
            context->manifest_path,
            sizeof(context->manifest_path))) {
        memset(context, 0, sizeof(*context));
        return false;
    }
#ifdef _WIN32
    {
        static const char suffix[] = ".recovered";
        size_t manifest_length = strlen(context->manifest_path);
        if (manifest_length + sizeof(suffix) > sizeof(context->tombstone_path)) {
            memset(context, 0, sizeof(*context));
            return false;
        }
        memcpy(context->tombstone_path, context->manifest_path, manifest_length);
        memcpy(context->tombstone_path + manifest_length, suffix, sizeof(suffix));
    }
#endif
    context->catalog_state.catalog = catalog;
    context->catalog_state.snapshot = snapshot;
    context->catalog_state.pager = pager;
    if (!tinydb_compact_v2_migration_catalog_state_is_valid(&context->catalog_state)) {
        memset(context, 0, sizeof(*context));
        return false;
    }
    return true;
}

static inline bool tinydb_compact_v2_migration_open_adapter_read_catalog(
    void* opaque,
    uint32_t table_id,
    uint32_t* root_page_num_out,
    uint64_t* schema_generation_out) {
    TinyDBCompactV2MigrationOpenAdapterContext* context =
        (TinyDBCompactV2MigrationOpenAdapterContext*)opaque;
    if (context == NULL) return false;
    return tinydb_compact_v2_migration_catalog_state_read(
        &context->catalog_state,
        table_id,
        root_page_num_out,
        schema_generation_out);
}

static inline bool tinydb_compact_v2_migration_open_adapter_is_authoritative_root(
    const TinyDBCompactV2MigrationOpenAdapterContext* context,
    uint32_t page_num) {
    const Catalog* catalog;
    if (context == NULL ||
        !tinydb_compact_v2_migration_catalog_state_is_valid(&context->catalog_state)) {
        return false;
    }
    catalog = context->catalog_state.catalog;
    for (uint32_t i = 0u; i < catalog->num_tables; i++) {
        if (catalog->schemas[i].root_page_num == page_num) return true;
    }
    return false;
}

static inline bool tinydb_compact_v2_migration_open_adapter_reclaim_staging(
    void* opaque,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count) {
    TinyDBCompactV2MigrationOpenAdapterContext* context =
        (TinyDBCompactV2MigrationOpenAdapterContext*)opaque;
    Pager* pager;
    if (context == NULL || claimed_pages == NULL || claimed_page_count == 0u ||
        !tinydb_compact_v2_migration_catalog_state_is_valid(&context->catalog_state)) {
        return false;
    }
    pager = (Pager*)context->catalog_state.pager;
    if (!pager->in_transaction) return false;
    for (uint32_t i = 0u; i < claimed_page_count; i++) {
        if (tinydb_compact_v2_migration_open_adapter_is_authoritative_root(
                context, claimed_pages[i])) {
            return false;
        }
    }
    return tinydb_compact_v2_migration_pager_reclaim_claims(
        pager, claimed_pages, claimed_page_count);
}

static inline bool tinydb_compact_v2_migration_open_adapter_reclaim_old_tree(
    void* opaque,
    Pager* pager,
    uint32_t old_root_page_num) {
    TinyDBCompactV2MigrationOpenAdapterContext* context =
        (TinyDBCompactV2MigrationOpenAdapterContext*)opaque;
    if (context == NULL || pager == NULL ||
        pager != context->catalog_state.pager || !pager->in_transaction ||
        !tinydb_compact_v2_migration_catalog_state_is_valid(&context->catalog_state)) {
        return false;
    }
    if (tinydb_compact_v2_migration_open_adapter_is_authoritative_root(
            context, old_root_page_num)) {
        return false;
    }
    return tinydb_fixed_v1_tree_reclaim(pager, old_root_page_num);
}

static inline bool tinydb_compact_v2_migration_open_adapter_remove_manifest(
    void* opaque) {
    TinyDBCompactV2MigrationOpenAdapterContext* context =
        (TinyDBCompactV2MigrationOpenAdapterContext*)opaque;
    if (context == NULL || context->manifest_path[0] == '\0') return false;
#ifdef _WIN32
    /*
     * MOVEFILE_WRITE_THROUGH makes disappearance of the active name the
     * durability boundary.  A leftover tombstone is harmless and can be
     * replaced by a later recovery attempt; correctness never depends on its
     * deletion succeeding.
     */
    (void)DeleteFileA(context->tombstone_path);
    if (!MoveFileExA(context->manifest_path,
                     context->tombstone_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    (void)DeleteFileA(context->tombstone_path);
    return true;
#else
    if (unlink(context->manifest_path) == 0) return true;
    return false;
#endif
}

#ifndef _WIN32
static inline bool tinydb_compact_v2_migration_open_adapter_parent_path(
    const char* database_filename,
    char* parent_out,
    size_t parent_capacity) {
    const char* slash;
    size_t length;
    if (parent_out == NULL || parent_capacity == 0u) return false;
    parent_out[0] = '\0';
    if (database_filename == NULL || database_filename[0] == '\0') return false;
    slash = strrchr(database_filename, '/');
    if (slash == NULL) {
        if (parent_capacity < 2u) return false;
        parent_out[0] = '.';
        parent_out[1] = '\0';
        return true;
    }
    if (slash == database_filename) {
        if (parent_capacity < 2u) return false;
        parent_out[0] = '/';
        parent_out[1] = '\0';
        return true;
    }
    length = (size_t)(slash - database_filename);
    if (length + 1u > parent_capacity) return false;
    memcpy(parent_out, database_filename, length);
    parent_out[length] = '\0';
    return true;
}
#endif

static inline bool tinydb_compact_v2_migration_open_adapter_sync_parent(
    void* opaque) {
    TinyDBCompactV2MigrationOpenAdapterContext* context =
        (TinyDBCompactV2MigrationOpenAdapterContext*)opaque;
    if (context == NULL || context->database_filename[0] == '\0') return false;
#ifdef _WIN32
    /* The active-name removal already used MOVEFILE_WRITE_THROUGH. */
    return true;
#else
    char parent[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    int fd;
    int sync_result;
    if (!tinydb_compact_v2_migration_open_adapter_parent_path(
            context->database_filename, parent, sizeof(parent))) {
        return false;
    }
#ifdef O_DIRECTORY
    fd = open(parent, O_RDONLY | O_DIRECTORY);
#else
    fd = open(parent, O_RDONLY);
#endif
    if (fd < 0) return false;
    sync_result = fsync(fd);
    if (close(fd) != 0) return false;
    return sync_result == 0;
#endif
}

static inline bool tinydb_compact_v2_migration_open_adapter_build(
    TinyDBCompactV2MigrationOpenAdapterContext* context,
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter_out) {
    if (adapter_out == NULL) return false;
    memset(adapter_out, 0, sizeof(*adapter_out));
    if (context == NULL ||
        !tinydb_compact_v2_migration_catalog_state_is_valid(&context->catalog_state)) {
        return false;
    }
    adapter_out->pager = (Pager*)context->catalog_state.pager;
    adapter_out->context = context;
    adapter_out->read_catalog = tinydb_compact_v2_migration_open_adapter_read_catalog;
    adapter_out->reclaim_staging_pages =
        tinydb_compact_v2_migration_open_adapter_reclaim_staging;
    adapter_out->reclaim_old_tree =
        tinydb_compact_v2_migration_open_adapter_reclaim_old_tree;
    adapter_out->remove_manifest = tinydb_compact_v2_migration_open_adapter_remove_manifest;
    adapter_out->sync_parent = tinydb_compact_v2_migration_open_adapter_sync_parent;
    return tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter_out);
}

#endif /* TINYDB_COMPACT_V2_MIGRATION_OPEN_ADAPTER_H */
