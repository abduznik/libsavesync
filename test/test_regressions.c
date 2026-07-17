#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "savesync.h"
#include "test_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ===================================================================
 *  Test Framework
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;
static int test_count = 0;

#define TEST_ASSERT(cond, msg) do { \
    test_count++; \
    if (!(cond)) { \
        printf("  FAIL [%d] %s\n", test_count, msg); \
        tests_failed++; \
    } else { \
        printf("  PASS [%d] %s\n", test_count, msg); \
        tests_passed++; \
    } \
} while(0)

static void write_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(content, 1, strlen(content), f); fclose(f); }
}

static bool file_contents_equal(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len != (long)strlen(expected)) { fclose(f); return false; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    bool eq = strcmp(buf, expected) == 0;
    free(buf);
    return eq;
}

/* ===================================================================
 *  Bug 1: Serialize pointer indirection
 *
 *  The original bug: serialize_reg() and serialize_entry() used &out
 *  (triple pointer) instead of out (double pointer) when writing the
 *  record header after the data. This would corrupt the buffer pointer
 *  and produce a garbled metadata file.
 *
 *  Regression test: write a registration with all fields populated,
 *  flush metadata, shut down, re-init, and verify every field survives
 *  the round-trip. If the header write is wrong, deserialization would
 *  read garbage and either crash or return wrong values.
 * =================================================================== */
void test_regression_serialize_pointer_indirection(void) {
    printf("--- Regression: serialize pointer indirection ---\n");

    char tmpdir[] = "/tmp/libsavesync_regr1_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a save file with all metadata fields populated */
    char save_path[4096];
    snprintf(save_path, sizeof(save_path), "%s/full_save.dat", tmpdir);
    write_test_file(save_path, "FULL_METADATA_TEST");

    sv_register_opts_t reg_opts = {
        .live_path = save_path,
        .platform = "ps2",
        .emulator = "pcsx2",
        .product_version = "2.0.0",
        .game_id = "TEST-00001",
        .rom_path = "/games/ff10.iso",
        .label = "Test Game Alpha",
        .shape = SV_SHAPE_FILE,
        .retention_count = 10,
    };
    sv_status_t reg_st;
    sv_registration_t *reg = sv_register(&reg_opts, &reg_st);
    TEST_ASSERT(reg != NULL, "registration with all fields succeeds");
    TEST_ASSERT(reg_st == SV_OK, "register returns OK");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    /* Create an entry too — exercises serialize_entry */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    TEST_ASSERT(save_res.entry_created, "entry created");

    /* Shutdown — triggers metadata_flush which calls both serializers */
    sv_shutdown();

    /* Re-init — triggers metadata_load which calls both deserializers */
    sv_init(base_path);

    /* Verify registration survived the round-trip */
    sv_registration_info_t reg_info;
    sv_status_t st = sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(st == SV_OK, "sv_read_registration after round-trip OK");
    TEST_ASSERT(strcmp(reg_info.live_path, save_path) == 0, "live_path survived round-trip");
    TEST_ASSERT(strcmp(reg_info.platform, "ps2") == 0, "platform survived round-trip");
    TEST_ASSERT(strcmp(reg_info.emulator, "pcsx2") == 0, "emulator survived round-trip");
    TEST_ASSERT(strcmp(reg_info.product_version, "2.0.0") == 0, "product_version survived round-trip");
    TEST_ASSERT(strcmp(reg_info.game_id, "TEST-00001") == 0, "game_id survived round-trip");
    TEST_ASSERT(strcmp(reg_info.rom_path, "/games/ff10.iso") == 0, "rom_path survived round-trip");
    TEST_ASSERT(strcmp(reg_info.label, "Test Game Alpha") == 0, "label survived round-trip");
    TEST_ASSERT(reg_info.shape == SV_SHAPE_FILE, "shape survived round-trip");
    TEST_ASSERT(reg_info.retention_count == 10, "retention_count survived round-trip");
    TEST_ASSERT(reg_info.entry_count == 1, "entry_count survived round-trip");

    /* Verify entry survived the round-trip */
    sv_id_t entry_ids[16];
    size_t entry_count = 0;
    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(st == SV_OK && entry_count == 1, "entry survived round-trip");

    sv_entry_info_t entry_info;
    st = sv_read_entry(entry_ids[0], &entry_info);
    TEST_ASSERT(st == SV_OK, "sv_read_entry after round-trip OK");
    TEST_ASSERT(entry_info.integrity_ok, "entry integrity_ok survived");
    TEST_ASSERT(entry_info.size_bytes == strlen("FULL_METADATA_TEST"), "entry size survived");
    TEST_ASSERT(strlen(entry_info.magazine_slot_path) > 0, "magazine path survived");

    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Bug 2: Opaque registration ID accessor
 *
 *  The original bug: test code accessed reg->id directly, which fails
 *  because sv_registration_t is an opaque struct. The fix was to add
 *  sv_registration_id() as the only public accessor.
 *
 *  Regression test: verify that sv_registration_id() returns a valid,
 *  non-zero ID, and that the ID is consistent across multiple calls.
 *  Also verify that read_registration returns the same ID.
 * =================================================================== */
void test_regression_opaque_registration_id_accessor(void) {
    printf("--- Regression: opaque registration ID accessor ---\n");

    char tmpdir[] = "/tmp/libsavesync_regr2_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_path[4096];
    snprintf(save_path, sizeof(save_path), "%s/save.dat", tmpdir);
    write_test_file(save_path, "ID_ACCESSOR_TEST");

    sv_register_opts_t reg_opts = {
        .live_path = save_path,
        .shape = SV_SHAPE_FILE,
    };
    sv_status_t reg_st;
    sv_registration_t *reg = sv_register(&reg_opts, &reg_st);
    TEST_ASSERT(reg != NULL, "register returns non-NULL");

    /* sv_registration_id must work and return consistent results */
    sv_id_t id1, id2;
    sv_registration_id(reg, id1);
    sv_registration_id(reg, id2);
    TEST_ASSERT(id1[0] != '\0', "sv_registration_id returns non-empty ID");
    TEST_ASSERT(memcmp(id1, id2, 8) == 0, "sv_registration_id is deterministic across calls");

    /* The ID from the accessor must match what read_registration reports */
    sv_registration_info_t reg_info;
    sv_status_t st = sv_read_registration(id1, &reg_info);
    TEST_ASSERT(st == SV_OK, "sv_read_registration OK with accessor-provided ID");
    TEST_ASSERT(memcmp(reg_info.id, id1, 8) == 0, "read_registration ID matches accessor ID");

    /* List registrations must also return the same ID */
    sv_id_t listed_ids[16];
    size_t count = 0;
    sv_list_registrations(listed_ids, 16, &count);
    TEST_ASSERT(count >= 1, "at least 1 registration listed");
    TEST_ASSERT(memcmp(listed_ids[0], id1, 8) == 0, "listed ID matches accessor ID");

    sv_unregister(reg);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Bug 3: Pull-select dangling pointer
 *
 *  The original bug: sv_pull_select() captured ent = find_entry(entry_id)
 *  before the backup save. The backup triggered retention, which could
 *  free ent. The code then used ent->magazine_slot_path on freed memory.
 *
 *  Regression test: set retention_count=2, create 2 entries (so backup
 *  triggers eviction), then pull_select with override. If the dangling
 *  pointer bug reappears, this would either crash or return wrong data.
 *
 *  We can't reliably detect use-after-free without ASan, but we can
 *  verify the pull produces correct content — which it can't if the
 *  pointer is stale.
 * =================================================================== */
void test_regression_pull_select_dangling_pointer(void) {
    printf("--- Regression: pull-select dangling pointer ---\n");

    char tmpdir[] = "/tmp/libsavesync_regr3_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_path[4096];
    snprintf(save_path, sizeof(save_path), "%s/dangle.dat", tmpdir);
    write_test_file(save_path, "DANGLE_V1");

    sv_register_opts_t reg_opts = {
        .live_path = save_path,
        .shape = SV_SHAPE_FILE,
        .retention_count = 2,
    };
    sv_status_t reg_st;
    sv_registration_t *reg = sv_register(&reg_opts, &reg_st);
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    /* Save V1 and V2 — now 2 entries, at retention cap */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);  /* V1 */

    write_test_file(save_path, "DANGLE_V2");
    sv_save(reg, NULL, &save_res);  /* V2 */

    sv_id_t entry_ids[16];
    size_t entry_count = 0;
    sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 2, "2 entries at retention cap");

    /* Now write V3 to live (creating conflict) and pull_select V1 with override.
     * The backup of V3 creates a 3rd entry, triggering retention eviction of V1.
     * If ent (V1) is used after eviction → dangling pointer (use-after-free).
     * With the fix, the code re-resolves ent after backup and gets NULL → clean NOT_FOUND. */
    write_test_file(save_path, "DANGLE_V3");

    sv_pull_opts_t pull_opts = { .on_conflict = SV_PULL_CONFLICT_OVERRIDE };
    sv_pull_result_t pull_res;

    /* Pull the OLDEST entry (entry_ids[0] = V1) with override.
     * V1 gets evicted during backup → find_entry returns NULL → SV_ERR_NOT_FOUND.
     * The critical assertion: no crash (use-after-free would crash under ASan),
     * and the error is NOT_FOUND (not IO, not CONFLICT, not garbage). */
    sv_status_t st = sv_pull_select(reg, entry_ids[0], &pull_opts, &pull_res);

    /* V1 was evicted during backup — clean NOT_FOUND is the correct outcome */
    TEST_ASSERT(st == SV_ERR_NOT_FOUND,
                "pull_select returns NOT_FOUND (not crash/garbage) when entry evicted during backup");
    TEST_ASSERT(!pull_res.did_pull, "did_pull is false on NOT_FOUND");

    /* Verify no memory corruption by performing another operation */
    sv_id_t post_ids[16];
    size_t post_count = 0;
    sv_list_entries(reg_id, post_ids, 16, &post_count);
    TEST_ASSERT(post_count == 2, "entries still consistent after failed pull_select");

    sv_unregister(reg);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Bug 4: Newest-entry tiebreak
 *
 *  The original bug: sv_pull() and sv_pull_select() found "newest"
 *  by comparing mtime only. With equal mtime (macOS 1s granularity),
 *  the loop `if (mtime > newest->mtime)` never fires, so entries[0]
 *  (the OLDEST by sequence) was selected instead of the true newest.
 *
 *  Regression test: force multiple entries to have identical mtime by
 *  creating them in rapid succession, then assert sv_pull selects the
 *  one with the highest sequence (last created), not entries[0].
 * =================================================================== */
void test_regression_newest_entry_tiebreak(void) {
    printf("--- Regression: newest-entry tiebreak ---\n");

    char tmpdir[] = "/tmp/libsavesync_regr4_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_path[4096];
    snprintf(save_path, sizeof(save_path), "%s/tiebreak.dat", tmpdir);
    write_test_file(save_path, "TIEBREAK_V1");

    sv_register_opts_t reg_opts = {
        .live_path = save_path,
        .shape = SV_SHAPE_FILE,
        .retention_count = 10,
    };
    sv_status_t reg_st;
    sv_registration_t *reg = sv_register(&reg_opts, &reg_st);
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    /* Save 3 entries rapidly — they'll likely have the same mtime on macOS */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);  /* V1, sequence=0 */

    write_test_file(save_path, "TIEBREAK_V2");
    sv_save(reg, NULL, &save_res);  /* V2, sequence=1 */

    write_test_file(save_path, "TIEBREAK_V3");
    sv_save(reg, NULL, &save_res);  /* V3, sequence=2 */

    /* Verify we have 3 entries */
    sv_id_t entry_ids[16];
    size_t entry_count = 0;
    sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 3, "3 entries created");

    /* sv_pull should select the entry with highest sequence (V3),
     * not entries[0] (which is V1 by creation order). */
    sv_pull_opts_t pull_opts = { .on_conflict = SV_PULL_CONFLICT_OVERRIDE };
    sv_pull_result_t pull_res;

    /* Modify live so pull has something to do */
    write_test_file(save_path, "LIVE_DIFFERENT");
    sv_pull(reg, &pull_opts, &pull_res);

    TEST_ASSERT(pull_res.did_pull, "sv_pull happened");
    /* If tiebreak is wrong, it would pull V1 (TIEBREAK_V1) instead of V3 */
    TEST_ASSERT(file_contents_equal(save_path, "TIEBREAK_V3"),
                "sv_pull selects V3 (highest sequence), not V1 (entries[0])");

    /* Also test pull_select's conflict check — it finds "latest" for conflict detection.
     * If the tiebreak is wrong there too, conflict detection would reference the wrong entry. */
    write_test_file(save_path, "LIVE_AGAIN_DIFFERENT");
    sv_pull_select(reg, entry_ids[0], &pull_opts, &pull_res);
    /* Pulling V1 should succeed and produce V1 content */
    TEST_ASSERT(file_contents_equal(save_path, "TIEBREAK_V1"),
                "sv_pull_select(entry_ids[0]) pulls V1 correctly");

    sv_unregister(reg);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void) {
    printf("\n=== libsavesync Regression Test Suite ===\n\n");

    test_regression_serialize_pointer_indirection();
    test_regression_opaque_registration_id_accessor();
    test_regression_pull_select_dangling_pointer();
    test_regression_newest_entry_tiebreak();

    printf("\n=== Results: %d passed, %d failed out of %d ===\n\n",
           tests_passed, tests_failed, test_count);

    return tests_failed > 0 ? 1 : 0;
}
