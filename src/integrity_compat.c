#include "diagnostics.h"

#include <stdio.h>

/*
 * Keep the historical db_integrity_check(Table*) ABI while routing the
 * implementation through the same fail-closed, try-pin-based whole-database
 * diagnostic used by SQL PRAGMA integrity_check.
 *
 * table.c still compiles its original implementation under the private
 * db_integrity_check_legacy_base name. This linked compatibility entry point
 * avoids process-fatal get_page() misses when the buffer pool is fully pinned
 * and strengthens the old API to validate every catalog root plus ownership.
 */
bool db_integrity_check(Table* table) {
    TinyDBPageOwnershipStats ownership;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];

    bool ok = tinydb_check_database(table,
                                    &ownership,
                                    message,
                                    sizeof(message));
    if (ok) {
        printf("ok\n");
        return true;
    }

    printf("Error: %s.\n",
           message[0] != '\0'
               ? message
               : "database integrity check failed");
    return false;
}
