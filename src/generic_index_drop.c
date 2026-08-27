#include "table.h"

#include <errno.h>
#include <stdio.h>

bool table_drop_index_legacy(Table* table, const char* index_name);

static bool build_range_filename(Table* table,
                                 const char* index_name,
                                 char* output,
                                 size_t output_size) {
    if (table == NULL || table->pager == NULL || index_name == NULL) return false;
    int written = snprintf(output,
                           output_size,
                           "%s.%s.idx.range",
                           table->pager->filename,
                           index_name);
    return written >= 0 && (size_t)written < output_size;
}

bool table_drop_index(Table* table, const char* index_name) {
    char range_filename[640];
    bool has_range_filename = build_range_filename(table,
                                                    index_name,
                                                    range_filename,
                                                    sizeof(range_filename));

    bool dropped = table_drop_index_legacy(table, index_name);
    if (!dropped) return false;

    if (has_range_filename &&
        remove(range_filename) != 0 && errno != ENOENT) {
        printf("Error: unable to remove generic range index snapshot.\n");
        return false;
    }
    return true;
}
