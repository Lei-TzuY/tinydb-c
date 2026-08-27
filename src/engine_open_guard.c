#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TinyDB* tinydb_open(const char* filename) {
    if (filename == NULL || filename[0] == '\0' ||
        strlen(filename) >= TINYDB_ENGINE_FILENAME_MAX) {
        return NULL;
    }

    TinyDB* database = (TinyDB*)calloc(1, sizeof(*database));
    if (database == NULL) return NULL;

    snprintf(database->filename, sizeof(database->filename), "%s", filename);
    database->table = db_open(filename);
    if (database->table == NULL) {
        free(database);
        return NULL;
    }

    if (!multitable_catalog_load(database->table, database->filename)) {
        printf("Error: schema catalog could not be loaded safely.\n");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }
    return database;
}
