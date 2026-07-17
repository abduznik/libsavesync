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

static void write_test_file_binary(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

/* ===================================================================
 *  PCSX2 (PS2) — SYSTEM.CNF serial extraction
 * =================================================================== */
void test_pcsx2_manifest(void) {
    printf("--- PCSX2 (PS2): SYSTEM.CNF serial ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_pcsx2_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a PS2 save directory with SYSTEM.CNF */
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/ps2_save", tmpdir);
    mkdir(save_dir, 0755);

    /* SYSTEM.CNF with PCSX2-style BOOT2 line */
    char cnf_path[4096];
    snprintf(cnf_path, sizeof(cnf_path), "%s/SYSTEM.CNF", save_dir);
    write_test_file(cnf_path, "BOOT2 = cdrom0:\\TEST_000.01;1\r\nVER = 2.00\r\nVMODE = NTSC\r\n");

    /* Load manifest — uses identity=none because libsavesync's pattern
     * matcher can't handle the cdrom0:\ prefix + semicolon suffix format.
     * Caller supplies the normalized serial as game_id. */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/pcsx2_folder.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "pcsx2_folder.cfg loads successfully");
    TEST_ASSERT(strcmp(sv_manifest_get_platform(manifest), "ps2") == 0, "platform is ps2");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_TEXT_PATTERN,
                "identity is text_pattern (auto-extracts serial from SYSTEM.CNF)");

    /* Register — identity should auto-detect game_id from SYSTEM.CNF */
    sv_register_opts_t opts = {
        .live_path = save_dir,
        .shape = SV_SHAPE_DIRECTORY,
        .retention_count = 5,
    };
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "pcsx2 register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    /* The pattern extracts "TEST_000.01" then normalization produces "TEST-00001" */
    TEST_ASSERT(strcmp(reg_info.game_id, "TEST-00001") == 0,
                "game_id auto-extracted and normalized to 'TEST-00001'");

    /* Verify template resolved correctly */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    TEST_ASSERT(save_res.entry_created, "save succeeds");

    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "TEST-00001") != NULL,
                "magazine path contains game_id");
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
 *  PPSSPP (PSP) — folder-based identification
 * =================================================================== */
void test_ppsspp_manifest(void) {
    printf("--- PPSSPP (PSP): directory-name pattern identification ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_ppsspp_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* --- Manifest loads with text_pattern identity --- */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/ppsspp.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "ppsspp.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_TEXT_PATTERN,
                "identity is text_pattern (directory-name pattern)");

    /* --- Auto-extract game_id from save slot directory --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/ULUS10509001", tmpdir);
        mkdir(save_dir, 0755);

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "ULUS10509001 register succeeds");

        sv_id_t rid; sv_registration_id(reg, rid);
        sv_registration_info_t ri; sv_read_registration(rid, &ri);
        TEST_ASSERT(strcmp(ri.game_id, "ULUS10509") == 0,
                    "game_id is 'ULUS10509' from save slot directory");
        sv_unregister(reg);
    }

    /* --- Different suffixes yield same game_id --- */
    {
        const char *suffixes[] = { "DAT", "DLC", "DLCBGM", "DLCTEX", "DLCVOICE", "SYSDATA" };
        for (size_t i = 0; i < sizeof(suffixes)/sizeof(suffixes[0]); i++) {
            char name[64];
            snprintf(name, sizeof(name), "ULUS10509%s", suffixes[i]);
            char save_dir[4096];
            snprintf(save_dir, sizeof(save_dir), "%s/%s", tmpdir, name);
            mkdir(save_dir, 0755);

            sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
            sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
            TEST_ASSERT(reg != NULL, "suffix register succeeds");

            sv_id_t rid; sv_registration_id(reg, rid);
            sv_registration_info_t ri; sv_read_registration(rid, &ri);
            char msg[128];
            snprintf(msg, sizeof(msg), "suffix '%s' yields game_id 'ULUS10509'", suffixes[i]);
            TEST_ASSERT(strcmp(ri.game_id, "ULUS10509") == 0, msg);
            sv_unregister(reg);
        }
    }

    /* --- EU region extracts same pattern shape --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/ULES01372001", tmpdir);
        mkdir(save_dir, 0755);

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "ULES01372001 register succeeds");

        sv_id_t rid; sv_registration_id(reg, rid);
        sv_registration_info_t ri; sv_read_registration(rid, &ri);
        TEST_ASSERT(strcmp(ri.game_id, "ULES01372") == 0,
                    "game_id is 'ULES01372' from EU directory");
        sv_unregister(reg);
    }

    /* --- Short name (< 9 chars) fails identity, game_id stays empty --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/SHORT", tmpdir);
        mkdir(save_dir, 0755);

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "short name registers (identity fails gracefully)");
        sv_id_t rid; sv_registration_id(reg, rid);
        sv_registration_info_t ri; sv_read_registration(rid, &ri);
        TEST_ASSERT(strlen(ri.game_id) == 0, "short name yields empty game_id");
        sv_unregister(reg);
    }

    /* --- Manifest round-trip --- */
    {
        char roundtrip_path[4096];
        snprintf(roundtrip_path, sizeof(roundtrip_path), "%s/ppsspp_rt.cfg", tmpdir);
        st = sv_manifest_save(roundtrip_path, manifest);
        TEST_ASSERT(st == SV_OK, "manifest round-trip saves");

        sv_manifest_t *m2 = sv_manifest_create();
        st = sv_manifest_load(roundtrip_path, m2);
        TEST_ASSERT(st == SV_OK, "manifest round-trip loads");
        TEST_ASSERT(sv_manifest_get_identity_method(m2) == SV_IDENTITY_TEXT_PATTERN,
                    "round-trip preserves identity=text_pattern");
        sv_manifest_free(m2);
    }

    /* --- Pull round-trip --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/ULUS10509002", tmpdir);
        mkdir(save_dir, 0755);

        char save_file[4096];
        snprintf(save_file, sizeof(save_file), "%s/ULUS10509002/ICON0.PNG", tmpdir);
        write_test_file(save_file, "PPSSPP_ICON_DATA");

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "pull test register succeeds");

        sv_save_result_t sr;
        sv_save(reg, NULL, &sr);
        TEST_ASSERT(sr.entry_created, "pull test save creates entry");

        sv_pull_result_t pr;
        sv_pull(reg, NULL, &pr);
        TEST_ASSERT(!pr.conflicted, "pull test no conflict");

        sv_unregister(reg);
    }

    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  DuckStation (PS1) — stem-based, no identity
 * =================================================================== */
void test_duckstation_manifest(void) {
    printf("--- DuckStation (PS1): stem-based, no identity ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_duck_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/FF7.mcd", tmpdir);
    write_test_file(save_file, "PS1_MEMCARD_DATA");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/duckstation.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "duckstation.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_NONE,
                "identity is none (stem-based, caller provides game_id)");

    /* Register with caller-supplied game_id */
    sv_register_opts_t opts = {
        .live_path = save_file,
        .game_id = "FF7",
        .shape = SV_SHAPE_FILE,
        .retention_count = 5,
    };
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "duckstation register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strcmp(reg_info.game_id, "FF7") == 0, "game_id matches caller-supplied value");

    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "FF7") != NULL,
                "magazine path contains game_id");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "ps1") != NULL,
                "magazine path contains platform");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Dolphin GC — header-based, no identity
 * =================================================================== */
void test_dolphin_gc_manifest(void) {
    printf("--- Dolphin GC: ROM header identity ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_dolphin_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a synthetic GC save file with game ID at offset 0 */
    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/TEST01.gci", tmpdir);
    write_test_file(save_file, "GCI_SAVE_DATA");

    char rom_path[4096];
    snprintf(rom_path, sizeof(rom_path), "%s/game.iso", tmpdir);
    uint8_t rom_data[256] = {0};
    memcpy(rom_data, "XXXX", 4);
    write_test_file_binary(rom_path, rom_data, sizeof(rom_data));

    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/dolphin_gc.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "dolphin_gc.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_ROM_HEADER,
                "identity is rom_header (reads 4 bytes from ROM header)");

    /* Register with ROM path — identity resolves from ROM header */
    sv_register_opts_t opts = {
        .live_path = save_file,
        .rom_path = rom_path,
        .shape = SV_SHAPE_FILE,
        .retention_count = 5,
    };
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "dolphin register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strlen(reg_info.game_id) > 0, "game_id resolved from ROM header");
    TEST_ASSERT(strncmp(reg_info.game_id, "XXXX", 4) == 0, "game_id is 'XXXX'");

    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "XXXX") != NULL,
                "magazine path contains game_id");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "gamecube") != NULL,
                "magazine path contains platform");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  RetroArch SNES — stem-based, no identity
 * =================================================================== */
void test_retroarch_snes_manifest(void) {
    printf("--- RetroArch SNES: stem-based, no identity ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_snes_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/Test Game Beta.srm", tmpdir);
    write_test_file(save_file, "SRM_DATA_HERE");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/retroarch_snes.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "retroarch_snes.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_NONE,
                "identity is none (stem-based, caller provides game_id)");

    sv_register_opts_t opts = {
        .live_path = save_file,
        .game_id = "Test Game Beta",
        .shape = SV_SHAPE_FILE,
        .retention_count = 5,
    };
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "retroarch_snes register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strcmp(reg_info.game_id, "Test Game Beta") == 0, "game_id matches");

    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);
    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "Test Game Beta") != NULL,
                "magazine path contains game_id");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "snes") != NULL,
                "magazine path contains platform");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  PCSX2 Legacy — shared memcard (Mcd001.ps2 flat file)
 * =================================================================== */
void test_pcsx2_file_manifest(void) {
    printf("--- PCSX2 Legacy: shared memcard ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_pcsx2leg_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a synthetic shared memcard file */
    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/Mcd001.ps2", tmpdir);
    write_test_file(save_file, "PS2_MEMCARD_IMAGE_DATA");

    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/pcsx2_file.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "pcsx2_file.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_NONE,
                "identity is none (shared memcard = opaque unit)");
    TEST_ASSERT(sv_manifest_get_shape(manifest) == SV_SHAPE_FILE,
                "shape is file (not directory)");

    /* Register as an opaque unit — caller supplies game_id */
    sv_register_opts_t opts = {
        .live_path = save_file,
        .game_id = "PS2_MEMCARD",
        .shape = SV_SHAPE_FILE,
        .retention_count = 3,
    };
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "pcsx2 legacy register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    TEST_ASSERT(strcmp(reg_info.game_id, "PS2_MEMCARD") == 0,
                "game_id matches caller-supplied value");

    /* Save and verify round-trip */
    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "save succeeds");
    TEST_ASSERT(save_res.entry_created, "entry created");

    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "PS2_MEMCARD") != NULL,
                "magazine path contains game_id");
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
 *  Ryujinx (Switch) — directory-based, title ID from ROM
 * =================================================================== */
void test_ryujinx_manifest(void) {
    printf("--- Ryujinx (Switch): text_pattern identity ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_ryujinx_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* Create a synthetic Ryujinx save directory structure:
     * save/{userId}/{slotId}/Player.sav — create parents first */
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s/save", tmpdir);
    mkdir(parent, 0755);
    snprintf(parent, sizeof(parent), "%s/save/0000000000000001", tmpdir);
    mkdir(parent, 0755);
    char save_dir[4096];
    snprintf(save_dir, sizeof(save_dir), "%s/save/0000000000000001/0", tmpdir);
    mkdir(save_dir, 0755);

    char sav_path[4096];
    snprintf(sav_path, sizeof(sav_path), "%s/Player.sav", save_dir);
    write_test_file(sav_path, "SWITCH_SAVE_DATA");

    /* Create a synthetic ROM with cnmt filename embedded in first 256KB */
    char rom_path[4096];
    snprintf(rom_path, sizeof(rom_path), "%s/game.nsp", tmpdir);
    uint8_t rom_data[262144];
    memset(rom_data, 0, sizeof(rom_data));
    /* Place cnmt filename at offset 1000 — fictional title ID 0100000000TEST00 */
    const char *cnmt_name = "0100000000TEST00.cnmt";
    memcpy(rom_data + 1000, cnmt_name, strlen(cnmt_name));
    write_test_file_binary(rom_path, rom_data, sizeof(rom_data));

    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/ryujinx.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "ryujinx.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_TEXT_PATTERN,
                "identity is text_pattern (cnmt scan)");
    TEST_ASSERT(sv_manifest_get_shape(manifest) == SV_SHAPE_DIRECTORY,
                "shape is directory");

    /* Register — identity should auto-detect title ID from cnmt pattern */
    sv_register_opts_t opts = {
        .live_path = save_dir,
        .rom_path = rom_path,
        .shape = SV_SHAPE_DIRECTORY,
        .retention_count = 5,
    };
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
    TEST_ASSERT(reg != NULL, "ryujinx register succeeds");

    sv_id_t reg_id;
    sv_registration_id(reg, reg_id);
    sv_registration_info_t reg_info;
    sv_read_registration(reg_id, &reg_info);
    /* Pattern extracts "0100000000TEST00" (14 hex chars after "01") */
    TEST_ASSERT(strlen(reg_info.game_id) > 0, "game_id auto-extracted from cnmt pattern");
    TEST_ASSERT(strncmp(reg_info.game_id, "0100000000TEST00", 16) == 0,
                "game_id is '0100000000TEST00' from cnmt pattern");

    /* Save and verify template resolution */
    sv_save_result_t save_res;
    st = sv_save(reg, NULL, &save_res);
    TEST_ASSERT(st == SV_OK, "save succeeds");
    TEST_ASSERT(save_res.entry_created, "entry created");

    sv_entry_info_t entry_info;
    sv_read_entry(save_res.entry_id, &entry_info);
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "0100000000TEST00") != NULL,
                "magazine path contains title ID");
    TEST_ASSERT(strstr(entry_info.magazine_slot_path, "switch") != NULL,
                "magazine path contains platform");

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  RPCS3 (PS3) — directory-name pattern identification
 * =================================================================== */
void test_rpcs3_manifest(void) {
    printf("--- RPCS3 (PS3): directory-name pattern identification ---\n");

    char tmpdir[] = "/tmp/libsavesync_real_rpcs3_XXXXXX";
    mkdtemp(tmpdir);
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/data", tmpdir);
    sv_init(base_path);

    /* --- Manifest loads with text_pattern identity --- */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_status_t st = sv_manifest_load("manifests/rpcs3.cfg", manifest);
    TEST_ASSERT(st == SV_OK, "rpcs3.cfg loads successfully");
    TEST_ASSERT(sv_manifest_get_identity_method(manifest) == SV_IDENTITY_TEXT_PATTERN,
                "identity is text_pattern (directory-name pattern)");

    /* --- Auto-extract game_id from save directory --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/BLUS30443DEMONSS005", tmpdir);
        mkdir(save_dir, 0755);

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "BLUS30443DEMONSS005 register succeeds");

        sv_id_t rid; sv_registration_id(reg, rid);
        sv_registration_info_t ri; sv_read_registration(rid, &ri);
        TEST_ASSERT(strcmp(ri.game_id, "BLUS30443") == 0,
                    "game_id is 'BLUS30443' from save directory");
        sv_unregister(reg);
    }

    /* --- Different suffixes yield same game_id --- */
    {
        const char *suffixes[] = { "SYSTEM", "F", "L01", "-SAW", "-NAUGHTYBEARSAVEGAME" };
        for (size_t i = 0; i < sizeof(suffixes)/sizeof(suffixes[0]); i++) {
            char name[64];
            snprintf(name, sizeof(name), "BLUS30826%s", suffixes[i]);
            char save_dir[4096];
            snprintf(save_dir, sizeof(save_dir), "%s/%s", tmpdir, name);
            mkdir(save_dir, 0755);

            sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
            sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
            TEST_ASSERT(reg != NULL, "suffix register succeeds");

            sv_id_t rid; sv_registration_id(reg, rid);
            sv_registration_info_t ri; sv_read_registration(rid, &ri);
            char msg[128];
            snprintf(msg, sizeof(msg), "suffix '%s' yields game_id 'BLUS30826'", suffixes[i]);
            TEST_ASSERT(strcmp(ri.game_id, "BLUS30826") == 0, msg);
            sv_unregister(reg);
        }
    }

    /* --- EU region extracts same pattern shape --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/BLES00676-SAW", tmpdir);
        mkdir(save_dir, 0755);

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "BLES00676-SAW register succeeds");

        sv_id_t rid; sv_registration_id(reg, rid);
        sv_registration_info_t ri; sv_read_registration(rid, &ri);
        TEST_ASSERT(strcmp(ri.game_id, "BLES00676") == 0,
                    "game_id is 'BLES00676' from EU directory");
        sv_unregister(reg);
    }

    /* --- Short name (< 9 chars) fails identity, game_id stays empty --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/SHORT", tmpdir);
        mkdir(save_dir, 0755);

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "short name registers (identity fails gracefully)");
        sv_id_t rid; sv_registration_id(reg, rid);
        sv_registration_info_t ri; sv_read_registration(rid, &ri);
        TEST_ASSERT(strlen(ri.game_id) == 0, "short name yields empty game_id");
        sv_unregister(reg);
    }

    /* --- Manifest round-trip --- */
    {
        char roundtrip_path[4096];
        snprintf(roundtrip_path, sizeof(roundtrip_path), "%s/rpcs3_rt.cfg", tmpdir);
        st = sv_manifest_save(roundtrip_path, manifest);
        TEST_ASSERT(st == SV_OK, "manifest round-trip saves");

        sv_manifest_t *m2 = sv_manifest_create();
        st = sv_manifest_load(roundtrip_path, m2);
        TEST_ASSERT(st == SV_OK, "manifest round-trip loads");
        TEST_ASSERT(sv_manifest_get_identity_method(m2) == SV_IDENTITY_TEXT_PATTERN,
                    "round-trip preserves identity=text_pattern");
        sv_manifest_free(m2);
    }

    /* --- Pull round-trip --- */
    {
        char save_dir[4096];
        snprintf(save_dir, sizeof(save_dir), "%s/BLUS30826F", tmpdir);
        mkdir(save_dir, 0755);

        char save_file[4096];
        snprintf(save_file, sizeof(save_file), "%s/BLUS30826F/USERDATA", tmpdir);
        write_test_file(save_file, "RPCS3_USERDATA");

        sv_register_opts_t opts = { .live_path = save_dir, .shape = SV_SHAPE_DIRECTORY, .retention_count = 5 };
        sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);
        TEST_ASSERT(reg != NULL, "pull test register succeeds");

        sv_save_result_t sr;
        sv_save(reg, NULL, &sr);
        TEST_ASSERT(sr.entry_created, "pull test save creates entry");

        sv_pull_result_t pr;
        sv_pull(reg, NULL, &pr);
        TEST_ASSERT(!pr.conflicted, "pull test no conflict");

        sv_unregister(reg);
    }

    sv_manifest_free(manifest);
    sv_shutdown();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void) {
    printf("\n=== libsavesync Real Manifest Integration Tests ===\n\n");

    test_pcsx2_manifest();
    test_pcsx2_file_manifest();
    test_ppsspp_manifest();
    test_rpcs3_manifest();
    test_duckstation_manifest();
    test_dolphin_gc_manifest();
    test_retroarch_snes_manifest();
    test_ryujinx_manifest();

    printf("\n=== Results: %d passed, %d failed out of %d ===\n\n",
           tests_passed, tests_failed, test_count);

    return tests_failed > 0 ? 1 : 0;
}
