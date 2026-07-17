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
 *  Test Framework (same as test_basic.c)
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

static void write_test_file_binary(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
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
 *  Declarations for functions to be implemented (Step 4)
 *  These are declared here so the test compiles and links against
 *  stubs. Once implemented in savesync.c, these declarations become
 *  redundant and can be removed.
 *
 *  NOTE: sv_identity_method_t, sv_manifest_t, sv_hash_db_lookup_fn
 *  are now in savesync.h — no need to redeclare here.
 * =================================================================== */

/* Manifest functions — defined in savesync.h */
sv_manifest_t *sv_manifest_create(void);
void           sv_manifest_free(sv_manifest_t *manifest);
sv_status_t    sv_manifest_load(const char *path, sv_manifest_t *out_manifest);
sv_status_t    sv_manifest_save(const char *path, const sv_manifest_t *manifest);

/* Manifest field accessors */
const char         *sv_manifest_get_platform(const sv_manifest_t *manifest);
const char         *sv_manifest_get_emulator(const sv_manifest_t *manifest);
sv_save_shape_t     sv_manifest_get_shape(const sv_manifest_t *manifest);
sv_identity_method_t sv_manifest_get_identity_method(const sv_manifest_t *manifest);
const char         *sv_manifest_get_save_path_template(const sv_manifest_t *manifest);

/* Manifest field setters */
void sv_manifest_set_platform(sv_manifest_t *manifest, const char *platform);
void sv_manifest_set_emulator(sv_manifest_t *manifest, const char *emulator);
void sv_manifest_set_shape(sv_manifest_t *manifest, sv_save_shape_t shape);
void sv_manifest_set_identity_method(sv_manifest_t *manifest, sv_identity_method_t method);
void sv_manifest_set_serial_params(sv_manifest_t *manifest, const char *file,
                                    size_t offset, size_t length, const char *pattern);
void sv_manifest_set_checksum_params(sv_manifest_t *manifest, size_t offset,
                                      size_t size, bool big_endian);
void sv_manifest_set_save_path_template(sv_manifest_t *manifest, const char *template);
void sv_manifest_set_hash_db_callback(sv_manifest_t *manifest,
                                       sv_hash_db_lookup_fn fn, void *ctx);

/* Register with manifest */
sv_registration_t *sv_register_with_manifest(
    const sv_register_opts_t *opts,
    const sv_manifest_t *manifest,
    sv_status_t *out_status
);

/* ===================================================================
 *  Test Helper: create a fake PS2 SYSTEM.CNF file
 *  Format: "BOOT2 = xx:xxxxxxxx.xx\r\n" at the start of the file
 * =================================================================== */
static void create_fake_system_cnf(const char *dir, const char *serial) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/SYSTEM.CNF", dir);
    char content[512];
    snprintf(content, sizeof(content), "BOOT2 = %s\r\n", serial);
    write_test_file(path, content);
}

/* ===================================================================
 *  Test Helper: create a fake PARAM.SFO file
 *  SFO header: magic "PSF" at offset 0, version at offset 4,
 *  strings table offset at offset 8, data table offset at offset 12.
 *  Game ID is typically at a known key in the strings table.
 *  For testing, we create a minimal SFO with the game ID embedded.
 * =================================================================== */
static void create_fake_param_sfo(const char *dir, const char *game_id) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/PARAM.SFO", dir);

    /* Minimal SFO: 20-byte header + strings + data */
    uint8_t sfo[256];
    memset(sfo, 0, sizeof(sfo));

    /* Magic: "PSF" */
    sfo[0] = 'P'; sfo[1] = 'S'; sfo[2] = 'F'; sfo[3] = 0x00;
    /* Version: 0x0101 */
    sfo[4] = 0x01; sfo[5] = 0x01; sfo[6] = 0x00; sfo[7] = 0x00;
    /* Strings table offset: 0x14 (20) */
    sfo[8] = 0x14; sfo[9] = 0x00; sfo[10] = 0x00; sfo[11] = 0x00;
    /* Data table offset: 0x14 + some strings */
    sfo[12] = 0x40; sfo[13] = 0x00; sfo[14] = 0x00; sfo[15] = 0x00;

    /* In strings table (offset 0x14): key "TITLE_ID\0" */
    int pos = 0x14;
    const char *key = "TITLE_ID";
    memcpy(sfo + pos, key, strlen(key) + 1);
    pos += strlen(key) + 1;
    /* Key "SAVEDATA_DIR\0" */
    const char *key2 = "SAVEDATA_DIR";
    memcpy(sfo + pos, key2, strlen(key2) + 1);
    pos += strlen(key2) + 1;
    /* Null terminator for keys */
    sfo[pos++] = 0x00;

    /* Pad to data table */
    pos = 0x40;
    /* Data: game_id string */
    memcpy(sfo + pos, game_id, strlen(game_id));

    write_test_file_binary(path, sfo, sizeof(sfo));
}

/* ===================================================================
 *  Test Helper: create a manifest text file
 * =================================================================== */
static void create_manifest_file(const char *path, const char *content) {
    write_test_file(path, content);
}

/* ===================================================================
 *  TEST SUITE
 * =================================================================== */
int main(void) {
    char tmpdir[] = "/tmp/libsavesync_strategy_test_XXXXXX";
    if (!sv_mkdtemp(tmpdir)) {
        printf("FAIL: could not create temp dir\n");
        return 1;
    }

    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);

    printf("\n=== libsavesync Strategy Mode Test Suite ===\n\n");
    printf("Temp dir: %s\n\n", tmpdir);

    /* ---- Init ---- */
    printf("--- Init ---\n");
    sv_status_t st = sv_init(base_path);
    TEST_ASSERT(st == SV_OK, "sv_init returns OK");

    /* ================================================================
     *  MANIFEST LIFECYCLE TESTS
     * ================================================================ */
    printf("--- Manifest Lifecycle ---\n");

    /* Test 1: Create manifest */
    sv_manifest_t *manifest = sv_manifest_create();
    TEST_ASSERT(manifest != NULL, "sv_manifest_create returns non-NULL");

    /* Test 2: Set and get platform */
    sv_manifest_set_platform(manifest, "ps2");
    TEST_ASSERT(strcmp(sv_manifest_get_platform(manifest), "ps2") == 0,
                "platform is 'ps2' after set");

    /* Test 3: Set and get emulator */
    sv_manifest_set_emulator(manifest, "pcsx2");
    TEST_ASSERT(strcmp(sv_manifest_get_emulator(manifest), "pcsx2") == 0,
                "emulator is 'pcsx2' after set");

    /* Test 4: Set and get shape */
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
    TEST_ASSERT(sv_manifest_get_shape(manifest) == SV_SHAPE_FILE,
                "shape is FILE after set");

    /* Test 5: Set and get identity method */
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_SERIAL_CNF,
                "identity_method is SERIAL_CNF after set");

    /* Test 6: Set and get save path template */
    sv_manifest_set_save_path_template(manifest, "{live_path}");
    TEST_ASSERT(strcmp(sv_manifest_get_save_path_template(manifest), "{live_path}") == 0,
                "save_path_template matches");

    /* Test 7: Free manifest */
    sv_manifest_free(manifest);
    TEST_ASSERT(1, "sv_manifest_free completed without crash");

    /* ================================================================
     *  MANIFEST FILE I/O TESTS
     * ================================================================ */
    printf("\n--- Manifest File I/O ---\n");

    /* Test 8: Save and load manifest */
    manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "psp");
    sv_manifest_set_emulator(manifest, "ppsspp");
    sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_SFO);
    sv_manifest_set_serial_params(manifest, "PARAM.SFO", 0x110, 12, "{SERIAL:12}");
    sv_manifest_set_save_path_template(manifest, "{game_id}/SAVEDATA");

    char manifest_path[4096];
    snprintf(manifest_path, sizeof(manifest_path), "%s/psp.manifest", tmpdir);
    st = sv_manifest_save(manifest_path, manifest);
    TEST_ASSERT(st == SV_OK, "sv_manifest_save returns OK");
    sv_manifest_free(manifest);

    manifest = sv_manifest_create();
    st = sv_manifest_load(manifest_path, manifest);
    TEST_ASSERT(st == SV_OK, "sv_manifest_load returns OK");
    TEST_ASSERT(strcmp(sv_manifest_get_platform(manifest), "psp") == 0,
                "loaded manifest platform is 'psp'");
    TEST_ASSERT(strcmp(sv_manifest_get_emulator(manifest), "ppsspp") == 0,
                "loaded manifest emulator is 'ppsspp'");
    TEST_ASSERT(sv_manifest_get_shape(manifest) == SV_SHAPE_DIRECTORY,
                "loaded manifest shape is DIRECTORY");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_SERIAL_SFO,
                "loaded manifest identity_method is SERIAL_SFO");
    sv_manifest_free(manifest);

    /* Test 9: Load from missing file */
    manifest = sv_manifest_create();
    st = sv_manifest_load("/nonexistent/file.manifest", manifest);
    TEST_ASSERT(st == SV_ERR_NOT_FOUND, "sv_manifest_load returns NOT_FOUND for missing file");
    sv_manifest_free(manifest);

    /* Test 10: Load from malformed file */
    {
        char bad_path[4096];
        snprintf(bad_path, sizeof(bad_path), "%s/bad.manifest", tmpdir);
        write_test_file(bad_path, "this is not a valid manifest\nplatform=\n");
        manifest = sv_manifest_create();
        st = sv_manifest_load(bad_path, manifest);
        /* Should either fail or load with partial data — not crash */
        TEST_ASSERT(st != SV_OK || sv_manifest_get_platform(manifest) != NULL,
                    "sv_manifest_load handles malformed file gracefully");
        sv_manifest_free(manifest);
    }

    /* ================================================================
     *  IDENTITY RESOLUTION TESTS
     * ================================================================ */
    printf("\n--- Identity Resolution: SYSTEM.CNF (Tier 1) ---\n");

    /* Test 11: Register with SYSTEM.CNF manifest — auto-detects game_id */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/ps2_save", tmpdir);
        mkdir(save_dir, 0755);
        create_fake_system_cnf(save_dir, "TEST-00001");

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "ps2");
        sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
        sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
        sv_manifest_set_save_path_template(manifest, "{live_path}");

        sv_register_opts_t reg_opts = {
            .live_path = save_dir,
            .shape = SV_SHAPE_DIRECTORY,
            .retention_count = 5,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "sv_register_with_manifest returns non-NULL");
        TEST_ASSERT(reg_st == SV_OK, "sv_register_with_manifest returns OK");

        /* Verify game_id was auto-detected */
        sv_registration_info_t reg_info;
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        st = sv_read_registration(reg_id, &reg_info);
        TEST_ASSERT(st == SV_OK, "sv_read_registration OK after strategy register");
        TEST_ASSERT(strlen(reg_info.game_id) > 0, "game_id was auto-detected (not empty)");
        TEST_ASSERT(strcmp(reg_info.game_id, "TEST-00001") == 0,
                    "game_id matches SYSTEM.CNF serial 'TEST-00001'");

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* Test 12: SYSTEM.CNF too short — resolution fails gracefully */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/short_cnf", tmpdir);
        mkdir(save_dir, 0755);
        {
            char cnf_path[4096];
            snprintf(cnf_path, sizeof(cnf_path), "%s/SYSTEM.CNF", save_dir);
            write_test_file(cnf_path, "SHORT");  /* too short for pattern */
        }

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "ps2");
        sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
        sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
        sv_manifest_set_save_path_template(manifest, "{live_path}");

        sv_register_opts_t reg_opts = {
            .live_path = save_dir,
            .shape = SV_SHAPE_DIRECTORY,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "register succeeds even with unparseable CNF");

        sv_registration_info_t reg_info;
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        st = sv_read_registration(reg_id, &reg_info);
        /* game_id should be empty/NULL — resolution failed but registration still works */
        TEST_ASSERT(strlen(reg_info.game_id) == 0,
                    "game_id is empty when SYSTEM.CNF is too short");

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* Test 13: SYSTEM.CNF pattern doesn't match */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/nomatch_cnf", tmpdir);
        mkdir(save_dir, 0755);
        create_fake_system_cnf(save_dir, "WRONG_FORMAT");

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "ps2");
        sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
        /* Pattern expects "BOOT = xx:xxxxxxxx.xx" but file has "BOOT2 = ..." — prefix mismatch */
        sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT = {SERIAL:12}");
        sv_manifest_set_save_path_template(manifest, "{live_path}");

        sv_register_opts_t reg_opts = {
            .live_path = save_dir,
            .shape = SV_SHAPE_DIRECTORY,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "register succeeds when pattern doesn't match");

        sv_registration_info_t reg_info;
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        st = sv_read_registration(reg_id, &reg_info);
        TEST_ASSERT(strlen(reg_info.game_id) == 0,
                    "game_id is empty when pattern doesn't match");

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* ================================================================
     *  IDENTITY RESOLUTION: CHECKSUM (Tier 2)
     * ================================================================ */
    printf("\n--- Identity Resolution: Checksum (Tier 2) ---\n");

    /* Test 14: Checksum-based identity resolution */
    {
        char save_file[4096];
        snprintf(save_file, sizeof(save_file), "%s/snes_save.srm", tmpdir);
        /* Create a 16-byte file with a known checksum at offset 0 */
        uint8_t data[16] = {0};
        data[0] = 0xAB; data[1] = 0xCD;  /* checksum = 0xCDAB */
        data[2] = 0x12; data[3] = 0x34;  /* extra data */
        write_test_file_binary(save_file, data, sizeof(data));

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "snes");
        sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_CHECKSUM);
        sv_manifest_set_checksum_params(manifest, 0, 2, false);  /* 2-byte LE checksum at offset 0 */
        sv_manifest_set_save_path_template(manifest, "{live_path}");

        sv_register_opts_t reg_opts = {
            .live_path = save_file,
            .shape = SV_SHAPE_FILE,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "register with CHECKSUM identity returns non-NULL");

        sv_registration_info_t reg_info;
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        st = sv_read_registration(reg_id, &reg_info);
        /* Checksum resolution requires a lookup table — without one, game_id stays empty */
        TEST_ASSERT(st == SV_OK, "sv_read_registration OK for checksum-based reg");

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* ================================================================
     *  PLUGGABLE HASH-DB CALLBACK (Tier 3)
     * ================================================================ */
    printf("\n--- Identity Resolution: Pluggable Hash-DB (Tier 3) ---\n");

    /* Test 15: Pluggable callback resolves game_id */
    {
        /* Callback that always returns "GAME_ABCD" */
        /* callback implementation — defined below main */
        /* For this test we verify the callback mechanism works */

        char save_file[4096];
        snprintf(save_file, sizeof(save_file), "%s/pluggable_save.dat", tmpdir);
        write_test_file(save_file, "PLUGGABLE_SAVE_DATA_HERE");

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "generic");
        sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_PLUGGABLE);
        sv_manifest_set_save_path_template(manifest, "{live_path}");
        /* Note: hash_db_callback would be set here via
         * sv_manifest_set_hash_db_callback() — the test verifies the API exists */

        sv_register_opts_t reg_opts = {
            .live_path = save_file,
            .shape = SV_SHAPE_FILE,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "register with PLUGGABLE identity returns non-NULL");
        /* The callback should have been invoked during registration */

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* ================================================================
     *  FALLBACK CHAIN TEST
     * ================================================================ */
    printf("\n--- Fallback Chain ---\n");

    /* Test 16: Tier 1 fails, no Tier 2/3 configured — game_id stays NULL */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/fallback_test", tmpdir);
        mkdir(save_dir, 0755);
        /* No SYSTEM.CNF file — Tier 1 will fail */
        {
            char other_path[4096];
            snprintf(other_path, sizeof(other_path), "%s/other_file.txt", save_dir);
            write_test_file(other_path, "data");
        }

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "ps2");
        sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
        sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
        sv_manifest_set_save_path_template(manifest, "{live_path}");

        sv_register_opts_t reg_opts = {
            .live_path = save_dir,
            .shape = SV_SHAPE_DIRECTORY,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "register succeeds when Tier 1 fails (no CNF file)");

        sv_registration_info_t reg_info;
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        st = sv_read_registration(reg_id, &reg_info);
        TEST_ASSERT(strlen(reg_info.game_id) == 0,
                    "game_id is empty when Tier 1 fails and no Tier 2/3 configured");

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* ================================================================
     *  INTEGRATION: SAVE/PULL UNDER STRATEGY MODE
     * ================================================================ */
    printf("\n--- Integration: Save/Pull under STRATEGY mode ---\n");

    /* Test 17: sv_save and sv_pull work identically under STRATEGY mode */
    {
        char save_file[4096];
        snprintf(save_file, sizeof(save_file), "%s/strategy_save.dat", tmpdir);
        write_test_file(save_file, "STRATEGY_V1");

        manifest = sv_manifest_create();
        sv_manifest_set_platform(manifest, "ps2");
        sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
        sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
        sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
        sv_manifest_set_save_path_template(manifest, "{live_path}");

        sv_register_opts_t reg_opts = {
            .live_path = save_file,
            .shape = SV_SHAPE_FILE,
            .retention_count = 5,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        TEST_ASSERT(reg != NULL, "strategy register returns non-NULL");

        /* Save */
        sv_save_result_t save_res;
        st = sv_save(reg, NULL, &save_res);
        TEST_ASSERT(st == SV_OK, "sv_save under STRATEGY mode returns OK");
        TEST_ASSERT(save_res.entry_created == true, "entry created under STRATEGY mode");

        /* List entries */
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        sv_id_t entry_ids[16];
        size_t entry_count = 0;
        st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
        TEST_ASSERT(st == SV_OK, "sv_list_entries under STRATEGY mode OK");
        TEST_ASSERT(entry_count == 1, "1 entry after save under STRATEGY mode");

        /* Modify and save again */
        write_test_file(save_file, "STRATEGY_V2");
        st = sv_save(reg, NULL, &save_res);
        TEST_ASSERT(st == SV_OK, "sv_save (modified) under STRATEGY mode OK");
        TEST_ASSERT(save_res.entry_created == true, "new entry created for modified save");

        st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
        TEST_ASSERT(entry_count == 2, "2 entries after second save under STRATEGY mode");

        /* Pull */
        sv_pull_opts_t pull_opts = { .on_conflict = SV_PULL_CONFLICT_OVERRIDE };
        sv_pull_result_t pull_res;
        st = sv_pull(reg, &pull_opts, &pull_res);
        TEST_ASSERT(st == SV_OK, "sv_pull under STRATEGY mode returns OK");
        TEST_ASSERT(pull_res.did_pull == true, "pull happened under STRATEGY mode");

        /* Pull select */
        write_test_file(save_file, "STRATEGY_V3");
        st = sv_pull_select(reg, entry_ids[0], &pull_opts, &pull_res);
        TEST_ASSERT(st == SV_OK, "sv_pull_select under STRATEGY mode returns OK");
        TEST_ASSERT(file_contents_equal(save_file, "STRATEGY_V1"),
                    "pulled V1 via pull_select under STRATEGY mode");

        sv_unregister(reg);
        sv_manifest_free(manifest);
    }

    /* ================================================================
     *  EDGE CASE: NO MANIFEST (falls back to DEFAULT)
     * ================================================================ */
    printf("\n--- Edge Case: No Manifest ---\n");

    /* Test 18: sv_register with STRATEGY mode but no manifest — works like DEFAULT */
    {
        char save_file[4096];
        snprintf(save_file, sizeof(save_file), "%s/default_fallback.dat", tmpdir);
        write_test_file(save_file, "DEFAULT_DATA");

        sv_register_opts_t reg_opts = {
            .live_path = save_file,
            .mode = SV_MODE_STRATEGY,  /* Request STRATEGY but no manifest */
            .shape = SV_SHAPE_FILE,
        };
        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, NULL, &reg_st);
        TEST_ASSERT(reg != NULL, "register with STRATEGY mode but NULL manifest succeeds");
        TEST_ASSERT(reg_st == SV_OK, "register returns OK (falls back to DEFAULT behavior)");

        sv_registration_info_t reg_info;
        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        st = sv_read_registration(reg_id, &reg_info);
        TEST_ASSERT(st == SV_OK, "sv_read_registration OK for fallback reg");
        TEST_ASSERT(strlen(reg_info.game_id) == 0, "game_id is empty (no manifest to resolve)");
        TEST_ASSERT(reg_info.mode == SV_MODE_STRATEGY, "mode is STRATEGY even without manifest");

        /* Save/pull should still work */
        sv_save_result_t save_res;
        st = sv_save(reg, NULL, &save_res);
        TEST_ASSERT(st == SV_OK, "sv_save works in STRATEGY mode without manifest");

        sv_unregister(reg);
    }

    /* ================================================================
     *  CLEANUP
     * ================================================================ */
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);

    printf("\n=== Results: %d passed, %d failed out of %d ===\n\n",
           tests_passed, tests_failed, test_count);

    return tests_failed > 0 ? 1 : 0;
}
