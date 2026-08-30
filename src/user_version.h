#ifndef TINYDB_USER_VERSION_H
#define TINYDB_USER_VERSION_H

#include "table.h"

#include <stddef.h>

/*
 * Non-fatal read companion to the historical db_get_user_version() ABI.
 *
 * The legacy value-only API cannot distinguish a genuine version 0 from a
 * buffer-pool acquisition failure. This status-returning seam leaves that ABI
 * untouched while giving SQL and external C callers a fail-closed path.
 * On failure, *version is reset to zero when version is non-NULL and message
 * receives a bounded diagnostic when a buffer is supplied.
 */
bool db_try_get_user_version(Table* table,
                             uint32_t* version,
                             char* message,
                             size_t message_size);

#endif /* TINYDB_USER_VERSION_H */
