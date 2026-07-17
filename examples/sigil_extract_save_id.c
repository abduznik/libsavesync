/*
 * sigil_extract_save_id.c — Minimal example: extract save_id from a ROM
 *
 * Shows how to use argosy-sigil's C API to pull the canonical game ID
 * from any supported ROM format. This is the first step in the
 * ROM → save pipeline before calling libsavesync.
 *
 * Build (POSIX):
 *   gcc -I../argosy-sigil/include sigil_extract_save_id.c
 *       -L../argosy-sigil/build -lsigil -o sigil_extract_save_id
 *
 * Build (MSVC + POSIX shim):
 *   cl /I../argosy-sigil/include /FIposix_compat.h /I<shim-dir>
 *      sigil_extract_save_id.c ../argosy-sigil/build/sigil.lib
 *
 * Usage:
 *   sigil_extract_save_id <rom-path> [platform]
 *
 * Examples:
 *   sigil_extract_save_id game.chd psp
 *   sigil_extract_save_id game.nsp
 *   sigil_extract_save_id game.bin psx
 */
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *platform_name(sigil_platform p) {
    switch (p) {
    case SIGIL_PLATFORM_PSP:      return "PSP";
    case SIGIL_PLATFORM_PSX:      return "PS1";
    case SIGIL_PLATFORM_PS2:      return "PS2";
    case SIGIL_PLATFORM_PSVITA:   return "PS Vita";
    case SIGIL_PLATFORM_SWITCH:   return "Switch";
    case SIGIL_PLATFORM_3DS:      return "3DS";
    case SIGIL_PLATFORM_WII:      return "Wii";
    case SIGIL_PLATFORM_WIIU:     return "Wii U";
    case SIGIL_PLATFORM_GAMECUBE: return "GameCube";
    case SIGIL_PLATFORM_PS3:      return "PS3";
    case SIGIL_PLATFORM_XBOX360:  return "Xbox 360";
    default:                      return "Unknown";
    }
}

static const char *usage_name(sigil_usage u) {
    switch (u) {
    case SIGIL_USAGE_FOLDER_EXACT:  return "folder-exact";
    case SIGIL_USAGE_FOLDER_PREFIX: return "folder-prefix";
    case SIGIL_USAGE_FILE_EXACT:    return "file-exact";
    case SIGIL_USAGE_FILE_PREFIX:   return "file-prefix";
    default:                         return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: sigil_extract_save_id <rom-path> [platform]\n"
            "\n"
            "Platforms: psp, psx, ps2, switch, 3ds, wii, wiiu, gamecube, psvita, ps3, xbox360\n"
            "Default: auto-detect from file extension\n"
            "\n"
            "Examples:\n"
            "  sigil_extract_save_id game.chd psp    # force PSP\n"
            "  sigil_extract_save_id game.nsp         # auto-detect Switch\n"
            "  sigil_extract_save_id game.bin psx     # force PS1\n"
            "\n"
            "Output:\n"
            "  save_id       = ULUS10167       (9-char prefix for save dirs/files)\n"
            "  title_id      = ULUS10167       (canonical game ID)\n"
            "  raw_serial    = ULUS-10167      (dashed form from disc)\n"
            "  platform      = PSP\n"
            "  usage         = folder-prefix   (saves use dir prefix convention)\n"
            "  source        = binary          (parsed from ROM structure)\n"
            "  experimental  = 0               (verified against real samples)\n"
            );
        return 1;
    }

    const char *rom_path = argv[1];
    sigil_platform hint = SIGIL_PLATFORM_AUTO;

    if (argc >= 3) {
        hint = sigil_platform_from_slug(argv[2]);
        if (hint == SIGIL_PLATFORM_AUTO && strcmp(argv[2], "auto") != 0) {
            fprintf(stderr, "Unknown platform: %s\n", argv[2]);
            return 1;
        }
    }

    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .flags = SIGIL_FLAG_FILENAME_FALLBACK
    };

    sigil_result result;
    int rc = sigil_extract_from_path(rom_path, hint, &opts, &result);

    if (rc != SIGIL_OK) {
        fprintf(stderr, "Error: %s (%s)\n", sigil_strerror(rc), rom_path);
        return 1;
    }

    /* Machine-readable output (parseable by scripts) */
    printf("save_id      = %s\n", result.save_id);
    printf("title_id     = %s\n", result.title_id);
    printf("raw_serial   = %s\n", result.raw_serial);
    printf("platform     = %s (%s)\n",
           platform_name(result.platform),
           sigil_platform_to_slug(result.platform));
    printf("usage        = %s\n", usage_name(result.usage));
    printf("source       = %s\n",
           result.source == SIGIL_SOURCE_BINARY ? "binary" : "filename");
    printf("experimental = %d\n", result.experimental);

    if (result.experimental) {
        fprintf(stderr, "\nWARNING: This extractor is experimental.\n"
                        "The save_id may be incorrect. Do not use for production saves.\n");
    }

    return 0;
}
