/*
 * sigil_ppsspp_demo.c — End-to-end integration: argosy-sigil + libsavesync
 *
 * Extracts the PSP save_id from a ROM via libsigil, scans a PPSSPP
 * save directory for matching folders, registers each with libsavesync
 * using the ppsspp.cfg manifest, and creates the first versioned snapshot.
 *
 * Build (MSVC + POSIX shim):
 *   cl /I<sigil>/include /I<savesync>/include /FI<shim>/posix_compat.h
 *      /I<shim> sigil_ppsspp_demo.c
 *      <sigil>/build/sigil.lib <savesync>/build/libsavesync.lib
 *
 * Usage:
 *   sigil_ppsspp_demo.exe [--rom=<path>] [--save-root=<path>]
 *                         [--manifest=<path>] [--db-path=<path>]
 *
 * Defaults:
 *   --rom        F:\\ROMS\\psp\\Marvel - Ultimate Alliance (USA) (v2.00).chd
 *   --save-root  F:\\EMULATORS\\ppsspp\\memstick\\PSP\\SAVEDATA
 *   --manifest   ..\\..\\libsavesync\\manifests\\ppsspp.cfg  (relative to exe)
 *   --db-path    .\\savesync_db  (current directory)
 */
#include "sigil.h"
#include "savesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#define PATH_SEP '/'
static void sleep_ms(unsigned ms) { struct timespec ts = { ms/1000, (ms%1000)*1000000 }; nanosleep(&ts, NULL); }
#endif

/* ------------------------------------------------------------------ */
/*  Config defaults                                                   */
/* ------------------------------------------------------------------ */
static const char *DEFAULT_ROM =
    "F:\\ROMS\\psp\\Marvel - Ultimate Alliance (USA) (v2.00).chd";
static const char *DEFAULT_SAVE_ROOT =
    "F:\\EMULATORS\\ppsspp\\memstick\\PSP\\SAVEDATA";
static const char *DEFAULT_MANIFEST =
    "..\\..\\libsavesync\\manifests\\ppsspp.cfg";
static const char *DEFAULT_DB_PATH = ".\\savesync_db";

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */
static int ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), fl = strlen(suffix);
    if (fl > sl) return 0;
    return _stricmp(s + sl - fl, suffix) == 0;
}

/* Find directories whose name starts with `prefix` under `root`.
 * Returns count of matches; names are written into out[][MAX_PATH]. */
static int scan_save_dirs(const char *root, const char *prefix,
                          char out[][1024], int max_out) {
    int count = 0;
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof(pattern), "%s\\*", root);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] == '.') continue;
            if (_strnicmp(fd.cFileName, prefix, strlen(prefix)) == 0) {
                if (count < max_out) {
                    snprintf(out[count], sizeof(out[count]), "%s\\%s", root, fd.cFileName);
                    count++;
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    const char *rom_path     = DEFAULT_ROM;
    const char *save_root    = DEFAULT_SAVE_ROOT;
    const char *manifest_path = DEFAULT_MANIFEST;
    const char *db_path      = DEFAULT_DB_PATH;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--rom=", 6) == 0)          rom_path = argv[i] + 6;
        else if (strncmp(argv[i], "--save-root=", 12) == 0) save_root = argv[i] + 12;
        else if (strncmp(argv[i], "--manifest=", 11) == 0)  manifest_path = argv[i] + 11;
        else if (strncmp(argv[i], "--db-path=", 10) == 0)   db_path = argv[i] + 10;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: sigil_ppsspp_demo [--rom=PATH] [--save-root=PATH]\n"
                   "       [--manifest=PATH] [--db-path=PATH]\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    printf("=== argosy-sigil + libsavesync Integration Demo ===\n\n");

    /* ---- Step 1: Extract save_id from ROM via sigil ---- */
    printf("[1] Extracting save_id from ROM...\n");
    printf("    ROM: %s\n", rom_path);

    sigil_result result;
    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .flags = SIGIL_FLAG_FILENAME_FALLBACK
    };
    int rc = sigil_extract_from_path(rom_path, SIGIL_PLATFORM_PSP, &opts, &result);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "    ERROR: sigil_extract_from_path failed: %s (%s)\n",
                sigil_strerror(rc), rom_path);
        return 1;
    }

    printf("    platform=%s\n", sigil_platform_to_slug(result.platform));
    printf("    title_id=%s\n", result.title_id);
    printf("    raw_serial=%s\n", result.raw_serial);
    printf("    save_id=%s\n", result.save_id);
    printf("    usage=%s\n", result.usage == SIGIL_USAGE_FOLDER_PREFIX ? "folder-prefix" : "?");
    printf("    source=%s\n", result.source == SIGIL_SOURCE_BINARY ? "binary" : "filename");
    printf("    experimental=%d\n", result.experimental);

    if (result.experimental) {
        fprintf(stderr, "\n    STOP: extraction is experimental — cannot proceed safely.\n");
        return 1;
    }

    /* ---- Step 2: Scan PPSSPP save directory for matching folders ---- */
    printf("\n[2] Scanning PPSSPP save directory...\n");
    printf("    Save root: %s\n", save_root);
    printf("    Prefix:    %s\n", result.save_id);

    char matches[64][1024];
    int match_count = scan_save_dirs(save_root, result.save_id, matches, 64);

    if (match_count == 0) {
        printf("    No save folders found matching prefix '%s'.\n", result.save_id);
        printf("    (This is expected if the game has never been run in PPSSPP.)\n");
        return 0;
    }

    printf("    Found %d matching save folder(s):\n", match_count);
    for (int i = 0; i < match_count; i++) {
        printf("      [%d] %s\n", i + 1, matches[i]);
    }

    /* ---- Step 3: Initialize libsavesync ---- */
    printf("\n[3] Initializing libsavesync (db: %s)...\n", db_path);
    sv_status_t st = sv_init(db_path);
    if (st != SV_OK) {
        fprintf(stderr, "    ERROR: sv_init failed (status=%d)\n", st);
        return 1;
    }
    printf("    sv_init OK\n");

    /* ---- Step 4: Load manifest ---- */
    printf("\n[4] Loading manifest: %s\n", manifest_path);
    sv_manifest_t *manifest = sv_manifest_create();
    st = sv_manifest_load(manifest_path, manifest);
    if (st != SV_OK) {
        fprintf(stderr, "    ERROR: sv_manifest_load failed (status=%d)\n", st);
        sv_manifest_free(manifest);
        return 1;
    }
    printf("    platform=%s  emulator=%s  shape=%d  identity=%d\n",
           sv_manifest_get_platform(manifest),
           sv_manifest_get_emulator(manifest),
           sv_manifest_get_shape(manifest),
           sv_manifest_get_identity_method(manifest));

    /* ---- Step 5: Register each save folder + create snapshot ---- */
    printf("\n[5] Registering save folders and creating snapshots...\n");

    for (int i = 0; i < match_count; i++) {
        printf("\n  --- Save folder %d/%d: %s ---\n", i + 1, match_count, matches[i]);

        sv_register_opts_t reg_opts = {
            .live_path       = matches[i],
            .platform        = "psp",
            .emulator        = "ppsspp",
            .game_id         = result.save_id,
            .rom_path        = rom_path,
            .shape           = SV_SHAPE_DIRECTORY,
            .retention_count = 5,
        };

        sv_status_t reg_st;
        sv_registration_t *reg = sv_register_with_manifest(&reg_opts, manifest, &reg_st);
        if (!reg || reg_st != SV_OK) {
            fprintf(stderr, "    Registration failed (status=%d)\n", reg_st);
            continue;
        }

        sv_id_t reg_id;
        sv_registration_id(reg, reg_id);
        printf("    Registered: reg_id=%s\n", reg_id);

        /* Read registration info to verify game_id */
        sv_registration_info_t reg_info;
        st = sv_read_registration(reg_id, &reg_info);
        if (st == SV_OK) {
            printf("    game_id=%s  live_path=%s\n", reg_info.game_id, reg_info.live_path);
        }

        /* Create first versioned snapshot */
        sv_save_result_t save_res;
        st = sv_save(reg, NULL, &save_res);
        if (st != SV_OK) {
            fprintf(stderr, "    sv_save failed (status=%d)\n", st);
            sv_unregister(reg);
            continue;
        }
        printf("    sv_save OK: entry_id=%s  created=%s  dedup_skipped=%s\n",
               save_res.entry_id,
               save_res.entry_created ? "true" : "false",
               save_res.dedup_skipped ? "true" : "false");

        /* List entries to confirm */
        sv_id_t entry_ids[16];
        size_t entry_count = 0;
        st = sv_list_entries(reg_id, entry_ids, 16, &entry_count);
        if (st == SV_OK) {
            printf("    sv_list_entries: %zu entry/entries\n", entry_count);
            for (size_t e = 0; e < entry_count; e++) {
                sv_entry_info_t einfo;
                if (sv_read_entry(entry_ids[e], &einfo) == SV_OK) {
                    printf("      entry[%zu]: id=%s  size=%llu  mtime=%lld\n",
                           e, einfo.id,
                           (unsigned long long)einfo.size_bytes,
                           (long long)einfo.mtime);
                }
            }
        }

        sv_unregister(reg);
    }

    /* ---- Cleanup ---- */
    sv_manifest_free(manifest);
    sv_shutdown();

    printf("\n=== Demo complete ===\n");
    return 0;
}
