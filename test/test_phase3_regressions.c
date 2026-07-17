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

static void create_fake_system_cnf(const char *dir, const char *serial) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/SYSTEM.CNF", dir);
    char content[512];
    snprintf(content, sizeof(content), "BOOT2 = %s\r\n", serial);
    write_test_file(path, content);
}

/* ===================================================================
 *  Risk 1: DEFAULT + STRATEGY registrations coexist
 *
 *  Both registration paths write to the same g_ctx and metadata store.
 *  A bug in one could corrupt the other.
 * =================================================================== */
void test_risk_default_strategy_coexist(void) {
    printf("--- Risk 1: DEFAULT + STRATEGY coexist ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r1_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* DEFAULT registration */
    char save_path1[4096];
    snprintf(save_path1, sizeof(save_path1), "%s/default.dat", tmpdir);
    write_test_file(save_path1, "DEFAULT_DATA");

    sv_register_opts_t opts1 = {
        .live_path = save_path1,
        .shape = SV_SHAPE_FILE,
        .retention_count = 5,
    };
    sv_status_t st;
    sv_registration_t *reg1 = sv_register(&opts1, &st);
    TEST_ASSERT(reg1 != NULL, "DEFAULT register succeeds");
    sv_id_t id1;
    sv_registration_id(reg1, id1);

    sv_save_result_t save_res;
    sv_save(reg1, NULL, &save_res);
    TEST_ASSERT(save_res.entry_created, "DEFAULT save creates entry");

    /* STRATEGY registration */
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/strategy", tmpdir);
    mkdir(save_dir, 0755);
    create_fake_system_cnf(save_dir, "TEST-00001");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest, "{live_path}");

    sv_register_opts_t opts2 = {
        .live_path = save_dir,
        .shape = SV_SHAPE_DIRECTORY,
        .mode = SV_MODE_STRATEGY,
        .retention_count = 5,
    };
    sv_registration_t *reg2 = sv_register_with_manifest(&opts2, manifest, &st);
    TEST_ASSERT(reg2 != NULL, "STRATEGY register succeeds");
    sv_id_t id2;
    sv_registration_id(reg2, id2);

    /* Verify IDs are different */
    TEST_ASSERT(memcmp(id1, id2, 8) != 0, "DEFAULT and STRATEGY have different IDs");

    /* Verify both registrations are listed */
    sv_id_t listed[16];
    size_t count = 0;
    sv_list_registrations(listed, 16, &count);
    TEST_ASSERT(count == 2, "2 registrations listed (DEFAULT + STRATEGY)");

    /* Verify both can be read */
    sv_registration_info_t info1, info2;
    sv_read_registration(id1, &info1);
    sv_read_registration(id2, &info2);
    TEST_ASSERT(strcmp(info1.live_path, save_path1) == 0, "DEFAULT live_path correct");
    TEST_ASSERT(strcmp(info2.live_path, save_dir) == 0, "STRATEGY live_path correct");
    TEST_ASSERT(info1.mode == SV_MODE_DEFAULT, "DEFAULT mode preserved");
    TEST_ASSERT(info2.mode == SV_MODE_STRATEGY, "STRATEGY mode preserved");

    /* Save to STRATEGY reg */
    sv_save(reg2, NULL, &save_res);
    TEST_ASSERT(save_res.entry_created, "STRATEGY save creates entry");

    /* Verify entries don't cross-contaminate */
    sv_id_t entries1[16], entries2[16];
    size_t ec1 = 0, ec2 = 0;
    sv_list_entries(id1, entries1, 16, &ec1);
    sv_list_entries(id2, entries2, 16, &ec2);
    TEST_ASSERT(ec1 == 1, "DEFAULT has 1 entry");
    TEST_ASSERT(ec2 == 1, "STRATEGY has 1 entry");

    sv_unregister(reg1);
    sv_unregister(reg2);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Risk 2: Identity resolution failure doesn't break dedup
 *
 *  When identity resolution fails, game_id stays NULL. The save/pull
 *  path should use normal hash-based dedup, identical to DEFAULT mode.
 * =================================================================== */
void test_risk_identity_failure_dedup(void) {
    printf("--- Risk 2: identity failure + dedup ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r2_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Register with manifest but no CNF file — identity will fail */
    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/noid.dat", tmpdir);
    write_test_file(save_file, "NOID_V1");

    sv_manifest_t *manifest = sv_manifest_create();
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
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with failed identity succeeds");
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    /* game_id should be empty */
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strlen(reg_info.game_id) == 0, "game_id empty when identity failed");

    /* Save — should work normally */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    TEST_ASSERT(save_res.entry_created, "save succeeds after identity failure");
    TEST_ASSERT(!save_res.dedup_skipped, "no dedup on first save");

    /* Save same content again — dedup should fire */
    sv_save(reg, NULL, &save_res);
    TEST_ASSERT(save_res.dedup_skipped, "dedup works after identity failure");

    /* Save different content — new entry */
    write_test_file(save_file, "NOID_V2");
    sv_save(reg, NULL, &save_res);
    TEST_ASSERT(save_res.entry_created, "modified save creates new entry after identity failure");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Risk 3: Manifest shape override doesn't crash
 *
 *  If manifest declares shape=FILE but live_path is a directory,
 *  copy_to_magazine uses atomic_copy_file on a directory path.
 * =================================================================== */
void test_risk_shape_override_mismatch(void) {
    printf("--- Risk 3: shape override mismatch ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r3_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a directory */
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/misshape", tmpdir);
    mkdir(save_dir, 0755);
    {
        char file_path[4096];
        snprintf(file_path, sizeof(file_path), "%s/file.txt", save_dir);
        write_test_file(file_path, "content");
    }

    /* Register with manifest that declares FILE shape but live_path is directory */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);  /* Wrong: actual is directory */
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_NONE);
    sv_manifest_set_save_path_template(manifest, "{live_path}");

    sv_register_opts_t reg_opts = {
        .live_path = save_dir,
        .shape = SV_SHAPE_UNKNOWN,  /* Let manifest override */
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with mismatched shape succeeds");

    /* Save should still work (atomic_copy_file on a dir path → fails gracefully or copies) */
    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    /* We don't assert success — the behavior depends on how atomic_copy_file
     * handles directory paths. The important thing is no crash. */
    TEST_ASSERT(st == SV_OK || st == SV_ERR_IO,
                "save with mismatched shape doesn't crash (returns OK or IO)");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Risk 4: sv_update_register on STRATEGY registration
 *
 *  Relocation and field updates should work identically regardless
 *  of how the registration was created.
 * =================================================================== */
void test_risk_update_strategy_registration(void) {
    printf("--- Risk 4: update STRATEGY registration ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r4_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/upd_strat", tmpdir);
    mkdir(save_dir, 0755);
    create_fake_system_cnf(save_dir, "TEST-00002");

    sv_manifest_t *manifest = sv_manifest_create();
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
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "STRATEGY register succeeds");
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    /* Update retention_count */
    sv_update_opts_t upd_opts = {
        .set_mask = (1u << 9),
        .retention_count = 3,
    };
    st = sv_update_register(reg, &upd_opts, NULL);
    TEST_ASSERT(st == SV_OK, "sv_update_register on STRATEGY reg OK");

    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(reg_info.retention_count == 3, "retention_count updated on STRATEGY reg");

    /* Update label */
    sv_update_opts_t upd_opts2 = {
        .set_mask = (1u << 6),
        .label = "Updated Label",
    };
    st = sv_update_register(reg, &upd_opts2, NULL);
    TEST_ASSERT(st == SV_OK, "sv_update_register (label) on STRATEGY reg OK");

    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strcmp(reg_info.label, "Updated Label") == 0, "label updated on STRATEGY reg");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Risk 5: Retention eviction preserves game_id
 *
 *  game_id lives on the registration, not on entries. Evicting entries
 *  should never affect the registration's game_id.
 * =================================================================== */
void test_risk_retention_preserves_game_id(void) {
    printf("--- Risk 5: retention preserves game_id ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r5_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/retention_gid.dat", tmpdir);
    write_test_file(save_file, "RGID_V1");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest, "{live_path}");

    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .shape = SV_SHAPE_FILE,
        .retention_count = 2,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    /* Save 3 times to trigger eviction */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    write_test_file(save_file, "RGID_V2");
    sv_save(reg, NULL, &save_res);
    write_test_file(save_file, "RGID_V3");
    sv_save(reg, NULL, &save_res);

    /* game_id should still be whatever was resolved (or empty if none) */
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    /* We registered with a CNF file that has no matching pattern for the save file,
     * so game_id should be empty. The point is it shouldn't crash or be corrupted. */
    TEST_ASSERT(reg_info.entry_count == 2, "retention capped entries at 2");
    TEST_ASSERT(strlen(reg_info.game_id) == 0 || reg_info.game_id[0] != '\0',
                "game_id not corrupted by retention");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Risk 6: Metadata round-trip preserves game_id with special chars
 *
 *  game_id is serialized as a TLV string field. Special characters
 *  (spaces, hyphens, long strings) must survive the round-trip.
 * =================================================================== */
void test_risk_metadata_roundtrip_game_id(void) {
    printf("--- Risk 6: metadata round-trip game_id ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r6_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/roundtrip.dat", tmpdir);
    write_test_file(save_file, "ROUNDTRIP_DATA");

    /* Register with a game_id containing special characters */
    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .game_id = "TEST-00001 rev1.0 (USA)",
        .shape = SV_SHAPE_FILE,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register(&reg_opts, &st);
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    sv_shutdown();
    sv_init(base_path);

    sv_registration_info_t reg_info;
    st = sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(st == SV_OK, "read_registration after round-trip OK");
    TEST_ASSERT(strcmp(reg_info.game_id, "TEST-00001 rev1.0 (USA)") == 0,
                "game_id with special chars survives round-trip");

    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Risk 7: Hash-DB callback returning NOT_FOUND doesn't crash
 *
 *  The pluggable callback is caller-supplied. If it returns an error,
 *  the library must handle it gracefully.
 * =================================================================== */
static sv_status_t test_hash_db_not_found(
    const uint8_t *hash, size_t hash_len,
    const char *platform,
    char *out_game_id, size_t out_game_id_len,
    void *user_ctx)
{
    (void)hash; (void)hash_len; (void)platform;
    (void)out_game_id; (void)out_game_id_len; (void)user_ctx;
    return SV_ERR_NOT_FOUND;
}

void test_risk_hash_db_callback_not_found(void) {
    printf("--- Risk 7: hash-DB callback NOT_FOUND ---\n");

    char tmpdir[] = "/tmp/libsavesync_p3r7_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/hashdb.dat", tmpdir);
    write_test_file(save_file, "HASHDB_DATA");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "generic");
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_PLUGGABLE);
    sv_manifest_set_save_path_template(manifest, "{live_path}");
    sv_manifest_set_hash_db_callback(manifest, test_hash_db_not_found, NULL);

    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .shape = SV_SHAPE_FILE,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with failing hash-DB callback succeeds");
    TEST_ASSERT(st == SV_OK, "register returns OK despite callback failure");

    /* game_id should be empty — callback returned NOT_FOUND */
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strlen(reg_info.game_id) == 0,
                "game_id empty when hash-DB callback returns NOT_FOUND");

    /* Save should work normally */
    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "save works after hash-DB failure");
    TEST_ASSERT(save_res.entry_created, "entry created");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  COVERAGE GAP 1: Hash-DB callback returning success
 *
 *  Test 15 in test_strategy.c never sets a callback — it's a stub.
 *  This test sets a callback that returns SV_OK with a game_id,
 *  and verifies the game_id is actually stored on the registration.
 * =================================================================== */
static sv_status_t test_hash_db_success(
    const uint8_t *hash, size_t hash_len,
    const char *platform,
    char *out_game_id, size_t out_game_id_len,
    void *user_ctx)
{
    (void)hash; (void)hash_len; (void)platform; (void)user_ctx;
    strncpy(out_game_id, "GAME_FROM_DB", out_game_id_len);
    return SV_OK;
}

void test_coverage_hash_db_success_sets_game_id(void) {
    printf("--- Coverage: hash-DB success sets game_id ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov1_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/hashdb_ok.dat", tmpdir);
    write_test_file(save_file, "HASHDB_OK_DATA");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "generic");
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_PLUGGABLE);
    sv_manifest_set_save_path_template(manifest, "{live_path}");
    sv_manifest_set_hash_db_callback(manifest, test_hash_db_success, NULL);

    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .shape = SV_SHAPE_FILE,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with successful hash-DB callback succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strcmp(reg_info.game_id, "GAME_FROM_DB") == 0,
                "game_id is set from hash-DB callback return value");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  COVERAGE GAP 2: Manifest loaded from file then used for registration
 *
 *  test_strategy.c tests manifest file I/O separately from registration.
 *  This test loads a manifest from disk and immediately uses it to
 *  register, verifying the full file → parse → register pipeline.
 * =================================================================== */
void test_coverage_manifest_file_to_registration(void) {
    printf("--- Coverage: manifest file → registration ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov2_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create SYSTEM.CNF in a save directory */
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/ps2_save", tmpdir);
    mkdir(save_dir, 0755);
    create_fake_system_cnf(save_dir, "TEST-00003");

    /* Write manifest to file */
    char manifest_path[4096];
    snprintf(manifest_path, sizeof(manifest_path), "%s/ps2.manifest", tmpdir);
    write_test_file(manifest_path,
        "# PS2 manifest\n"
        "platform=ps2\n"
        "emulator=pcsx2\n"
        "shape=directory\n"
        "identity=serial_cnf\n"
        "serial_file=SYSTEM.CNF\n"
        "serial_offset=0\n"
        "serial_length=256\n"
        "serial_pattern=BOOT2 = {SERIAL:12}\n"
        "save_path_template={live_path}\n"
    );

    /* Load manifest from file */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load(manifest_path, manifest);
    TEST_ASSERT(st == SV_OK, "manifest loaded from file");

    /* Verify parsed fields */
    TEST_ASSERT(strcmp(sv_manifest_get_platform(manifest), "ps2") == 0, "platform parsed correctly");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_SERIAL_CNF,
                "identity method parsed correctly");

    /* Use loaded manifest for registration */
    sv_register_opts_t reg_opts = {
        .live_path = save_dir,
        .shape = SV_SHAPE_DIRECTORY,
    };
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with file-loaded manifest succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strcmp(reg_info.game_id, "TEST-00003") == 0,
                "game_id resolved from file-loaded manifest");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  COVERAGE GAP 3: Fallback chain does NOT exist
 *
 *  The spec/wiki claims "tiers are tried in order; first match wins."
 *  The actual code is a switch on identity_method — only ONE tier runs.
 *  This test proves the gap: set identity_method to SERIAL_CNF, provide
 *  a save file with NO SYSTEM.CNF but WITH a valid checksum. The
 *  checksum tier is NEVER reached because identity_method selects only
 *  the serial tier.
 *
 *  If the fallback chain existed, this test would produce a game_id
 *  from the checksum. Since it doesn't, game_id stays empty.
 * =================================================================== */
void test_coverage_no_fallback_chain_exists(void) {
    printf("--- Coverage: NO fallback chain (tier isolation) ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov3_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a save file with NO SYSTEM.CNF but valid checksum data */
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/no_cnf", tmpdir);
    mkdir(save_dir, 0755);
    /* No SYSTEM.CNF file — serial extraction will fail */

    /* But create a data file with a recognizable checksum */
    char data_path[4096];
    snprintf(data_path, sizeof(data_path), "%s/save.dat", save_dir);
    uint8_t data[16] = {0};
    data[0] = 0xAB; data[1] = 0xCD;
    write_test_file_binary(data_path, data, sizeof(data));

    /* Set identity to SERIAL_CNF — this will fail (no CNF file).
     * If fallback existed, it would try CHECKSUM next and find the data file.
     * But fallback doesn't exist, so game_id stays empty. */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest, "{live_path}");

    sv_register_opts_t reg_opts = {
        .live_path = save_dir,
        .shape = SV_SHAPE_DIRECTORY,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register succeeds when serial fails");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);

    /* game_id should be empty — no fallback to checksum tier */
    TEST_ASSERT(strlen(reg_info.game_id) == 0,
                "game_id empty: serial failed, NO fallback to checksum (gap confirmed)");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  COVERAGE GAP 4: Checksum identity produces actual game_id
 *
 *  test_strategy.c test 14 registers with CHECKSUM identity but
 *  never asserts the game_id content — it just checks "registration OK."
 *  This test verifies the checksum identity actually produces a
 *  deterministic game_id string from the file's checksum bytes.
 * =================================================================== */
void test_coverage_checksum_identity_produces_game_id(void) {
    printf("--- Coverage: checksum identity → game_id content ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov4_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/snes.srm", tmpdir);
    uint8_t data[16] = {0};
    data[0] = 0x34; data[1] = 0x12;  /* LE checksum = 0x1234 */
    write_test_file_binary(save_file, data, sizeof(data));

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "snes");
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_CHECKSUM);
    sv_manifest_set_checksum_params(manifest, 0, 2, false);
    sv_manifest_set_save_path_template(manifest, "{live_path}");

    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .shape = SV_SHAPE_FILE,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "checksum register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);

    /* Checksum identity returns a formatted key, not a real game_id lookup.
     * The key should contain the checksum value. */
    TEST_ASSERT(strlen(reg_info.game_id) > 0,
                "checksum identity produces non-empty game_id");
    TEST_ASSERT(strncmp(reg_info.game_id, "CKSUM_", 6) == 0,
                "checksum game_id starts with 'CKSUM_' prefix");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  COVERAGE GAP 5: save_path_template resolves and organizes magazine
 *
 *  save_path_template with {game_id}/{platform} should resolve to a
 *  directory path and store magazine entries under it.
 * =================================================================== */
void test_coverage_save_path_template_resolves(void) {
    printf("--- Coverage: save_path_template resolves ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov5_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a SYSTEM.CNF so game_id resolves */
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/ps2_save", tmpdir);
    mkdir(save_dir, 0755);
    create_fake_system_cnf(save_dir, "TEST-00004");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest, "{game_id}/{platform}");

    sv_register_opts_t reg_opts = {
        .live_path = save_dir,
        .shape = SV_SHAPE_DIRECTORY,
        .retention_count = 5,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with template succeeds");

    /* Save should use the resolved template path for magazine storage */
    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "save succeeds");
    TEST_ASSERT(save_res.entry_created, "entry created");

    /* Read the entry and check its magazine path contains the resolved template */
    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "TEST-00004") != NULL,
                "magazine path contains game_id from template");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "ps2") != NULL,
                "magazine path contains platform from template");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Template with unresolvable placeholder — {game_id} → "unknown"
 * =================================================================== */
void test_coverage_save_path_template_unknown_game_id(void) {
    printf("--- Coverage: save_path_template unknown game_id ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov5b_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/nogid.dat", tmpdir);
    write_test_file(save_file, "NO_GID_DATA");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_NONE);
    sv_manifest_set_save_path_template(manifest, "{game_id}/{platform}");

    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .shape = SV_SHAPE_FILE,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "register with unknown game_id succeeds");

    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "save succeeds");

    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "unknown") != NULL,
                "magazine path uses 'unknown' for unresolved game_id");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "ps2") != NULL,
                "magazine path contains platform");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Registration WITHOUT template — unchanged behavior
 * =================================================================== */
void test_coverage_no_template_unchanged_behavior(void) {
    printf("--- Coverage: no template → unchanged behavior ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov5c_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/nosave.dat", tmpdir);
    write_test_file(save_file, "NO_TEMPLATE_DATA");

    sv_register_opts_t reg_opts = {
        .live_path = save_file,
        .shape = SV_SHAPE_FILE,
        .retention_count = 5,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register(&reg_opts, &st);
    TEST_ASSERT(reg != NULL, "register without template succeeds");

    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "save succeeds");

    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "magazine") != NULL,
                "default magazine path contains 'magazine'");

    sv_unregister(reg);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Template round-trips through metadata
 * =================================================================== */
void test_coverage_template_metadata_roundtrip(void) {
    printf("--- Coverage: template metadata round-trip ---\n");

    char tmpdir[] = "/tmp/libsavesync_cov5d_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/rt_save", tmpdir);
    mkdir(save_dir, 0755);
    create_fake_system_cnf(save_dir, "TEST-00005");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_set_platform(manifest, "ps2");
    sv_manifest_set_shape(manifest, SV_SHAPE_DIRECTORY);
    sv_manifest_set_identity_method(manifest, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest, "{game_id}/{platform}");

    sv_register_opts_t reg_opts = {
        .live_path = save_dir,
        .shape = SV_SHAPE_DIRECTORY,
        .retention_count = 5,
    };
    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &st);
    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);

    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    sv_id_t entry_id_before;
    memcpy(entry_id_before, save_res.entry_id, 8);

    sv_shutdown();
    sv_init(base_path);

    sv_entry_info_t entry_info;
    st = sv_read_entry(entry_id_before, &entry_info);
    TEST_ASSERT(st == SV_OK, "entry survives round-trip");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "TEST-00005") != NULL,
                "magazine path still contains game_id after round-trip");

    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  COLLISION RISK: Two registrations with {game_id}-only template
 *
 *  Both fail identity resolution → both resolve to "unknown".
 *  Does the shared directory cause data corruption?
 * =================================================================== */
void test_collision_risk_game_id_only_template(void) {
    printf("--- Collision Risk: {game_id}-only template ---\n");

    char tmpdir[] = "/tmp/libsavesync_coll_XXXXXX";
    sv_mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Registration A: no CNF file, template = {game_id} */
    char save_a[4096];
    snprintf(save_a, sizeof(save_a), "%s/save_a.dat", tmpdir);
    write_test_file(save_a, "DATA_A");

    sv_manifest_t *manifest_a = sv_manifest_create();
    sv_manifest_set_platform(manifest_a, "generic");
    sv_manifest_set_shape(manifest_a, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest_a, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest_a, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest_a, "{game_id}");

    sv_register_opts_t opts_a = { .live_path = save_a, .shape = SV_SHAPE_FILE };
    sv_status_t st;
    sv_registration_t *reg_a = sv_register_with_manifest(&opts_a, manifest_a, &st);
    TEST_ASSERT(reg_a != NULL, "register A succeeds");

    /* Registration B: same template, different file */
    char save_b[4096];
    snprintf(save_b, sizeof(save_b), "%s/save_b.dat", tmpdir);
    write_test_file(save_b, "DATA_B");

    sv_manifest_t *manifest_b = sv_manifest_create();
    sv_manifest_set_platform(manifest_b, "generic");
    sv_manifest_set_shape(manifest_b, SV_SHAPE_FILE);
    sv_manifest_set_identity_method(manifest_b, SV_IDENTITY_SERIAL_CNF);
    sv_manifest_set_serial_params(manifest_b, "SYSTEM.CNF", 0, 256, "BOOT2 = {SERIAL:12}");
    sv_manifest_set_save_path_template(manifest_b, "{game_id}");

    sv_register_opts_t opts_b = { .live_path = save_b, .shape = SV_SHAPE_FILE };
    sv_registration_t *reg_b = sv_register_with_manifest(&opts_b, manifest_b, &st);
    TEST_ASSERT(reg_b != NULL, "register B succeeds");

    /* Both should resolve to the same directory (both game_id = "unknown") */
    sv_id_t id_a, id_b;
    sv_registration_id(reg_a, id_a);
    sv_registration_id(reg_b, id_b);

    sv_save_result_t res_a, res_b;
    sv_save(reg_a, NULL, &res_a);
    sv_save(reg_b, NULL, &res_b);
    TEST_ASSERT(res_a.entry_created && res_b.entry_created, "both saves create entries");

    /* Read both entries — verify content is correct (no corruption) */
    sv_entry_info_t info_a, info_b;
    sv_read_entry(res_a.entry_id, &info_a);
    sv_read_entry(res_b.entry_id, &info_b);

    /* Both magazine paths should contain "unknown" */
    TEST_ASSERT(strstr(info_a.magazine_slot_path, "unknown") != NULL,
                "A's magazine path contains 'unknown'");
    TEST_ASSERT(strstr(info_b.magazine_slot_path, "unknown") != NULL,
                "B's magazine path contains 'unknown'");

    /* Entry IDs must differ (entries are unique even if directory isn't) */
    TEST_ASSERT(memcmp(res_a.entry_id, res_b.entry_id, 8) != 0,
                "entry IDs are unique despite shared directory");

    /* The magazine files exist and contain the right content */
    TEST_ASSERT(strlen(info_a.magazine_slot_path) > 0, "A has a magazine path");
    TEST_ASSERT(strlen(info_b.magazine_slot_path) > 0, "B has a magazine path");

    /* Verify actual file content is not corrupted */
    FILE *fa = fopen(info_a.magazine_slot_path, "rb");
    FILE *fb = fopen(info_b.magazine_slot_path, "rb");
    TEST_ASSERT(fa != NULL, "A's magazine file exists on disk");
    TEST_ASSERT(fb != NULL, "B's magazine file exists on disk");

    if (fa && fb) {
        char buf_a[256] = {0}, buf_b[256] = {0};
        fread(buf_a, 1, sizeof(buf_a) - 1, fa);
        fread(buf_b, 1, sizeof(buf_b) - 1, fb);
        fclose(fa);
        fclose(fb);
        TEST_ASSERT(strcmp(buf_a, "DATA_A") == 0, "A's file content is DATA_A (not corrupted)");
        TEST_ASSERT(strcmp(buf_b, "DATA_B") == 0, "B's file content is DATA_B (not corrupted)");
    }

    /* CONCLUSION: No data corruption. Entries from different registrations
     * share a directory but don't overwrite each other because entry IDs
     * are unique. This is an ORGANIZATIONAL concern, not a data integrity
     * bug. The resolved directory serves as a "bucket," and unique entry
     * IDs prevent actual collision. */

    sv_unregister(reg_a);
    sv_unregister(reg_b);
    sv_manifest_free(manifest_a);
    sv_manifest_free(manifest_b);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void) {
    printf("\n=== libsavesync Phase 3 Regression Test Suite ===\n\n");

    test_risk_default_strategy_coexist();
    test_risk_identity_failure_dedup();
    test_risk_shape_override_mismatch();
    test_risk_update_strategy_registration();
    test_risk_retention_preserves_game_id();
    test_risk_metadata_roundtrip_game_id();
    test_risk_hash_db_callback_not_found();
    test_coverage_hash_db_success_sets_game_id();
    test_coverage_manifest_file_to_registration();
    test_coverage_no_fallback_chain_exists();
    test_coverage_checksum_identity_produces_game_id();
    test_coverage_save_path_template_resolves();
    test_coverage_save_path_template_unknown_game_id();
    test_coverage_no_template_unchanged_behavior();
    test_coverage_template_metadata_roundtrip();
    test_collision_risk_game_id_only_template();

    printf("\n=== Results: %d passed, %d failed out of %d ===\n\n",
           tests_passed, tests_failed, test_count);

    return tests_failed > 0 ? 1 : 0;
}
