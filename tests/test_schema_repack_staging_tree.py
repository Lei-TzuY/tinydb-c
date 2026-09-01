import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "schema_repack_staging_tree.h"

static void set_column(TableColumn* column,
                       const char* name,
                       ColumnType type,
                       uint32_t offset,
                       uint32_t size) {
    memset(column, 0, sizeof(*column));
    snprintf(column->name, sizeof(column->name), "%s", name);
    column->type = type;
    column->offset = offset;
    column->size = size;
}

static void make_schema(TableSchema* schema, uint32_t title_size, uint32_t body_size) {
    memset(schema, 0, sizeof(*schema));
    snprintf(schema->name, sizeof(schema->name), "%s", "archive");
    schema->num_columns = 3u;
    set_column(&schema->columns[0], "id", COL_TYPE_INT, 0u, 4u);
    set_column(&schema->columns[1], "title", COL_TYPE_VARCHAR, 4u, title_size);
    set_column(&schema->columns[2], "body", COL_TYPE_VARCHAR, 4u + title_size, body_size);
    schema->row_size = 4u + title_size + body_size;
}

static void make_payload(const TableSchema* schema,
                         uint32_t id,
                         TinyDBRecordPayload* payload) {
    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, &id, sizeof(id));
    snprintf((char*)payload->bytes + schema->columns[1].offset,
             schema->columns[1].size,
             "title-%u",
             id);
    snprintf((char*)payload->bytes + schema->columns[2].offset,
             schema->columns[2].size,
             "body-%u",
             id);
}

static int verify_rows(const TinyDBCompactV2StagingLeafChain* chain,
                       const TableSchema* schema,
                       uint32_t expected_rows) {
    uint32_t expected_key = 1u;
    for (uint32_t page_index = 0u; page_index < chain->page_count; page_index++) {
        const unsigned char* page = tinydb_compact_v2_staging_page_const(chain, page_index);
        uint32_t count = 0u;
        if (page == NULL || !tinydb_leaf_page_count(page, PAGE_SIZE, &count)) return 1;
        for (uint32_t cell = 0u; cell < count; cell++) {
            uint32_t key = 0u;
            const void* value = NULL;
            uint32_t value_length = 0u;
            TinyDBRecordPayload payload;
            char expected_title[32];
            char expected_body[32];
            if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, cell, &key) ||
                !tinydb_leaf_page_value_at(page, PAGE_SIZE, cell, &value, &value_length) ||
                key != expected_key ||
                !tinydb_row_envelope_decode_compact_v2(schema,
                                                        (const unsigned char*)value,
                                                        value_length,
                                                        &payload)) {
                return 2;
            }
            snprintf(expected_title, sizeof(expected_title), "title-%u", key);
            snprintf(expected_body, sizeof(expected_body), "body-%u", key);
            if (strcmp((const char*)payload.bytes + schema->columns[1].offset,
                       expected_title) != 0 ||
                strcmp((const char*)payload.bytes + schema->columns[2].offset,
                       expected_body) != 0) {
                return 3;
            }
            expected_key++;
        }
    }
    return expected_key == expected_rows + 1u ? 0 : 4;
}

static int build_multi_leaf(void) {
    TableSchema source_schema;
    TableSchema destination_schema;
    TinyDBRecordPayload payload;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackStagingTreeResult result;
    unsigned char leaf_images[16u * PAGE_SIZE];
    unsigned char internal_images[16u * PAGE_SIZE];
    const uint32_t leaf_pages[16] = {
        101u, 102u, 103u, 104u, 105u, 106u, 107u, 108u,
        109u, 110u, 111u, 112u, 113u, 114u, 115u, 116u
    };
    const uint32_t internal_pages[16] = {
        201u, 202u, 203u, 204u, 205u, 206u, 207u, 208u,
        209u, 210u, 211u, 212u, 213u, 214u, 215u, 216u
    };
    char message[192];

    make_schema(&source_schema, 32u, 64u);
    make_schema(&destination_schema, 128u, 256u);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves,
                                                    leaf_images,
                                                    leaf_pages,
                                                    16u) ||
        !tinydb_schema_repack_staging_init(&staging,
                                           &source_schema,
                                           &destination_schema,
                                           &leaves,
                                           message,
                                           sizeof(message))) return 1;

    for (uint32_t id = 1u; id <= 120u; id++) {
        make_payload(&source_schema, id, &payload);
        if (!tinydb_schema_repack_staging_visit(&source_schema, &payload, &staging)) {
            fprintf(stderr, "multi-leaf row %u failed: %s\n", id, staging.message);
            return 2;
        }
    }
    if (leaves.page_count < 2u) return 3;
    if (!tinydb_schema_repack_staging_build_tree(&staging,
                                                 120u,
                                                 &hierarchy,
                                                 internal_images,
                                                 internal_pages,
                                                 16u,
                                                 &result,
                                                 message,
                                                 sizeof(message))) {
        fprintf(stderr, "tree build failed: %s\n", message);
        return 4;
    }
    if (!result.ready || result.row_count != 120u ||
        result.leaf_page_count != leaves.page_count ||
        result.internal_page_count == 0u || result.level_count == 0u ||
        result.root_page_num != hierarchy.root_page_num ||
        !tinydb_schema_repack_staging_tree_validate_result(
            &staging, &hierarchy, &result) ||
        !tinydb_compact_v2_staging_hierarchy_validate(&hierarchy)) return 5;
    int verified = verify_rows(&leaves, &destination_schema, 120u);
    return verified == 0 ? 0 : 10 + verified;
}

static int build_single_leaf(void) {
    TableSchema source_schema;
    TableSchema destination_schema;
    TinyDBRecordPayload payload;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackStagingTreeResult result;
    unsigned char leaf_image[PAGE_SIZE];
    const uint32_t leaf_page[1] = {301u};
    char message[192];

    make_schema(&source_schema, 32u, 64u);
    make_schema(&destination_schema, 128u, 256u);
    memset(leaf_image, 0, sizeof(leaf_image));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves,
                                                    leaf_image,
                                                    leaf_page,
                                                    1u) ||
        !tinydb_schema_repack_staging_init(&staging,
                                           &source_schema,
                                           &destination_schema,
                                           &leaves,
                                           message,
                                           sizeof(message))) return 1;
    make_payload(&source_schema, 1u, &payload);
    if (!tinydb_schema_repack_staging_visit(&source_schema, &payload, &staging)) return 2;
    if (!tinydb_schema_repack_staging_build_tree(&staging,
                                                 1u,
                                                 &hierarchy,
                                                 NULL,
                                                 NULL,
                                                 0u,
                                                 &result,
                                                 message,
                                                 sizeof(message))) return 3;
    if (!result.ready || result.root_page_num != 301u ||
        result.leaf_page_count != 1u || result.internal_page_count != 0u ||
        result.level_count != 0u || result.row_count != 1u ||
        !tinydb_schema_repack_staging_tree_validate_result(&staging, NULL, &result)) {
        return 4;
    }
    return 0;
}

static int reject_page_namespace_overlap(void) {
    TableSchema source_schema;
    TableSchema destination_schema;
    TinyDBRecordPayload payload;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackStagingTreeResult result;
    unsigned char leaf_images[8u * PAGE_SIZE];
    unsigned char internal_images[8u * PAGE_SIZE];
    const uint32_t leaf_pages[8] = {401u, 402u, 403u, 404u, 405u, 406u, 407u, 408u};
    const uint32_t overlapping_internal_pages[8] = {
        401u, 502u, 503u, 504u, 505u, 506u, 507u, 508u
    };
    char message[192];

    make_schema(&source_schema, 32u, 64u);
    make_schema(&destination_schema, 128u, 256u);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves,
                                                    leaf_images,
                                                    leaf_pages,
                                                    8u) ||
        !tinydb_schema_repack_staging_init(&staging,
                                           &source_schema,
                                           &destination_schema,
                                           &leaves,
                                           message,
                                           sizeof(message))) return 1;
    for (uint32_t id = 1u; id <= 80u; id++) {
        make_payload(&source_schema, id, &payload);
        if (!tinydb_schema_repack_staging_visit(&source_schema, &payload, &staging)) return 2;
    }
    memset(&result, 0xA5, sizeof(result));
    if (tinydb_schema_repack_staging_build_tree(&staging,
                                                80u,
                                                &hierarchy,
                                                internal_images,
                                                overlapping_internal_pages,
                                                8u,
                                                &result,
                                                message,
                                                sizeof(message))) return 3;
    if (result.ready || result.root_page_num != 0u || result.row_count != 0u ||
        hierarchy.built) return 4;
    if (strstr(message, "hierarchy") == NULL) return 5;
    return 0;
}

int main(void) {
    int rc = build_multi_leaf();
    if (rc != 0) return 10 + rc;
    rc = build_single_leaf();
    if (rc != 0) return 40 + rc;
    rc = reject_page_namespace_overlap();
    if (rc != 0) return 70 + rc;
    puts("SCHEMA_REPACK_STAGING_TREE_OK");
    return 0;
}
'''


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src = os.path.join(repo, "src")
    support_sources = [
        os.path.join(src, "slotted_leaf_v2.c"),
        os.path.join(src, "leaf_format.c"),
        os.path.join(src, "leaf_page_access.c"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w", newline="\n") as handle:
            handle.write(PROBE)

        if os.name == "nt":
            compiler = shutil.which("cl")
            if compiler is None:
                print("SKIP: MSVC compiler not available")
                return 0
            exe = os.path.join(tmp, "probe.exe")
            command = [
                compiler,
                "/nologo",
                "/std:c11",
                "/W4",
                "/WX",
                f"/I{src}",
                probe,
                *support_sources,
                f"/Fe:{exe}",
            ]
        else:
            compiler = shutil.which("gcc") or shutil.which("cc")
            if compiler is None:
                print("SKIP: C compiler not available")
                return 0
            exe = os.path.join(tmp, "probe")
            command = [
                compiler,
                "-std=c11",
                "-D_XOPEN_SOURCE=700",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                f"-I{src}",
                probe,
                *support_sources,
                "-o",
                exe,
            ]

        build = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if build.returncode != 0:
            sys.stdout.write(build.stdout)
            sys.stderr.write(build.stderr)
            return build.returncode

        run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        if run.returncode != 0:
            return run.returncode
        if "SCHEMA_REPACK_STAGING_TREE_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
