#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "savesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

int main(void) {
    char tmpdir[] = "/tmp/libsavesync_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("FAIL: could not create temp dir\n");
        return 1;
    }

    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);

    printf("\n=== libsavesync Basic Test Suite ===\n\n");
    printf("Temp dir: %s\n\n", tmpdir);

    sv_id_t reg_id;

    /* ---- Init ---- */
    printf("--- Init ---\n");
    sv_status_t st = sv_init(base_path);
    TEST_ASSERT(st == SV_OK, "sv_init returns OK");

    /* ---- Test 1: Register a file save ---- */
    printf("\n--- Register File Save ---\n");
    char save_path[4096];
    snprintf(save_path, sizeof(save_path), "%s/savefile.srm", tmpdir);
    write_test_file(save_path, "SAVE_DATA_VERSION_1");

    sv_register_opts_t reg_opts = {
        .live_path = save_path,
        .shape = SV_SHAPE_FILE,
        .retention_count = 5,
    };
    sv_status_t reg_st;
    sv_registration_t *reg = sv_register(&reg_opts, &reg_st);
    TEST_ASSERT(reg != NULL, "sv_register returns non-NULL handle");
    TEST_ASSERT(reg_st == SV_OK, "sv_register returns OK");

    sv_registration_id(reg, reg_id);

    /* ---- Test 2: List registrations ---- */
    printf("\n--- List Registrations ---\n");
    sv_id_t reg_ids[16];
    size_t reg_count = 0;
    st = sv_list_registrations(reg_ids, 16, &reg_count);
    TEST_ASSERT(st == SV_OK, "sv_list_registrations returns OK");
    TEST_ASSERT(reg_count == 1, "registration count is 1");
    TEST_ASSERT(memcmp(reg_ids[0], reg_id, 8) == 0, "listed ID matches handle ID");

    /* ---- Test 3: Read registration ---- */
    printf("\n--- Read Registration ---\n");
    sv_registration_info_t reg_info;
    st = sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(st == SV_OK, "sv_read_registration returns OK");
    TEST_ASSERT(strcmp(reg_info.live_path, save_path) == 0, "live_path matches");
    TEST_ASSERT(reg_info.shape == SV_SHAPE_FILE, "shape is FILE");
    TEST_ASSERT(reg_info.entry_count == 0, "entry_count is 0");

    /* ---- Test 4: Save ---- */
    printf("\n--- Save ---\n");
    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "sv_save returns OK");
    TEST_ASSERT(save_res.entry_created == true, "entry was created");
    TEST_ASSERT(save_res.dedup_skipped == false, "no dedup on first save");
    TEST_ASSERT(save_res.evicted_count == 0, "no evictions");

    st = sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(st == SV_OK, "sv_read_registration after save OK");
    TEST_ASSERT(reg_info.entry_count == 1, "entry_count is 1 after save");

    /* ---- Test 5: List entries ---- */
    printf("\n--- List Entries ---\n");
    sv_id_t entry_ids[16];
    size_t entry_count = 0;
    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(st == SV_OK, "sv_list_entries returns OK");
    TEST_ASSERT(entry_count == 1, "entry count is 1");
    TEST_ASSERT(memcmp(entry_ids[0], save_res.entry_id, 8) == 0, "listed entry ID matches save result");

    /* ---- Test 6: Read entry ---- */
    printf("\n--- Read Entry ---\n");
    sv_entry_info_t entry_info;
    st = sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(st == SV_OK, "sv_read_entry returns OK");
    TEST_ASSERT(entry_info.integrity_ok == true, "integrity is OK");
    TEST_ASSERT(entry_info.size_bytes == strlen("SAVE_DATA_VERSION_1"), "size matches");
    TEST_ASSERT(memcmp(entry_info.parent_id, reg_id, 8) == 0, "parent_id matches reg ID");
    TEST_ASSERT(entry_info.shape == SV_SHAPE_FILE, "shape is FILE");
    TEST_ASSERT(strlen(entry_info.magazine_slot_path) > 0, "magazine_slot_path is set");

    /* ---- Test 7: Save again (same content = dedup) ---- */
    printf("\n--- Dedup Test ---\n");
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "sv_save (dedup) returns OK");
    TEST_ASSERT(save_res.dedup_skipped == true, "dedup skipped (no change)");
    TEST_ASSERT(save_res.entry_created == false, "no entry created (dedup)");

    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 1, "still 1 entry after dedup save");

    /* ---- Test 8: Modify file, save again ---- */
    printf("\n--- Modified Save ---\n");
    write_test_file(save_path, "SAVE_DATA_VERSION_2");
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "sv_save (modified) returns OK");
    TEST_ASSERT(save_res.entry_created == true, "entry created after modification");
    TEST_ASSERT(save_res.dedup_skipped == false, "no dedup (content changed)");

    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 2, "2 entries after second save");

    /* ---- Test 9: Force save (identical content) ---- */
    printf("\n--- Force Save ---\n");
    sv_save_opts_t force_opts = { .force = true };
    st = sv_save(reg, &force_opts, &save_res);
    TEST_ASSERT(st == SV_OK, "sv_save (force) returns OK");
    TEST_ASSERT(save_res.entry_created == true, "force creates new entry despite identical hash");
    TEST_ASSERT(save_res.dedup_skipped == false, "no dedup (forced)");

    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 3, "3 entries after force save");

    /* ---- Test 10: Pull_select an older entry ---- */
    printf("\n--- Pull/Select Test ---\n");
    write_test_file(save_path, "SAVE_DATA_VERSION_3");

    sv_pull_opts_t pull_opts = { .on_conflict = SV_PULL_CONFLICT_OVERRIDE };
    sv_pull_result_t pull_res;

    write_test_file(save_path, "SAVE_DATA_VERSION_4");

    st = sv_pull_select(reg, entry_ids[1], &pull_opts, &pull_res);
    TEST_ASSERT(st == SV_OK, "sv_pull_select (override) returns OK");
    TEST_ASSERT(pull_res.did_pull == true, "pull actually happened");
    TEST_ASSERT(file_contents_equal(save_path, "SAVE_DATA_VERSION_2"),
                "pulled content is V2");

    /* ---- Test 11: Retention eviction ---- */
    printf("\n--- Retention Eviction ---\n");
    sv_update_opts_t upd_opts = {
        .set_mask = (1u << 9),
        .retention_count = 2,
    };
    st = sv_update_register(reg, &upd_opts, NULL);
    TEST_ASSERT(st == SV_OK, "sv_update_register (set retention=2) OK");

    write_test_file(save_path, "SAVE_DATA_VERSION_5");
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "sv_save (trigger retention) OK");
    TEST_ASSERT(save_res.evicted_count >= 1, "at least 1 entry evicted");

    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 2, "2 entries remain after retention eviction");

    /* ---- Test 12: Default pull ---- */
    printf("\n--- Default Pull ---\n");
    /* Save V6 cleanly (no conflict, just a regular save) */
    write_test_file(save_path, "SAVE_DATA_VERSION_6");
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "sv_save (save V6) returns OK");

    /* Now write V7 to live_path (create a difference) */
    write_test_file(save_path, "SAVE_DATA_VERSION_7");

    /* Pull with REPORT should detect conflict */
    sv_pull_opts_t pull_opts_report = { .on_conflict = SV_PULL_CONFLICT_REPORT };
    st = sv_pull(reg, &pull_opts_report, &pull_res);
    TEST_ASSERT(st == SV_ERR_CONFLICT, "sv_pull (report) returns CONFLICT when live changed");
    TEST_ASSERT(pull_res.conflicted == true, "conflict flag set");

    /* Pull with OVERRIDE should succeed and pull V6 back */
    st = sv_pull(reg, &pull_opts, &pull_res);
    TEST_ASSERT(st == SV_OK, "sv_pull (override) returns OK");
    TEST_ASSERT(pull_res.did_pull == true, "pull happened");
    TEST_ASSERT(file_contents_equal(save_path, "SAVE_DATA_VERSION_6"),
                "pulled content is V6 (latest saved version)");

    /* ---- Test 13: Unregister (orphans entries, doesn't delete them) ---- */
    printf("\n--- Unregister (Orphaning) ---\n");
    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    size_t pre_unreg_count = entry_count;

    sv_unregister(reg);
    TEST_ASSERT(1, "sv_unregister completed");

    st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == 0, "no entries listed under now-gone reg");

    sv_id_t zero_id = {0};
    st = sv_list_entries(zero_id, entry_ids, 16, &entry_count);
    TEST_ASSERT(entry_count == pre_unreg_count, "entries appear as orphans after unregister");

    /* ---- Shutdown ---- */
    sv_shutdown();

    /* ---- Cleanup ---- */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);

    printf("\n=== Results: %d passed, %d failed out of %d ===\n\n",
           tests_passed, tests_failed, test_count);

    return tests_failed > 0 ? 1 : 0;
}
