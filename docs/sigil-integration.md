# argosy-sigil Integration

## Overview

[argosy-sigil](https://github.com/rommforge/argosy-sigil) is a companion library that extracts platform-native title IDs from ROM files. It reads the binary structure of disc images (ISO9660, NSP, NCA, etc.) to pull the canonical game identifier — the same identifier that emulator save directories are named after.

libsavesync's manifest-driven identity resolution (Tier 1: serial extraction from save files) handles identification *after* a save is on disk. Sigil handles identification *before* — given a ROM, tell me which game it is so I can find its saves.

**Together they form a complete ROM → save pipeline:**

```
ROM file ──→ argosy-sigil ──→ save_id (e.g. ULUS10167)
                                    │
                                    ▼
                         Scan SAVEDATA/ for folders
                         starting with that prefix
                                    │
                                    ▼
                         libsavesync ──→ sv_register_with_manifest()
                                         sv_save() → versioned snapshot
```

## Supported Platforms (sigil)

| Platform | Format | Identity Source | Notes |
|----------|--------|-----------------|-------|
| PSP | `.cso`, `.ciso`, `.chd`, `.iso` | `UMD_DATA.BIN` | CHD opens via libchdr; `.chd` is valid for PSP dumps |
| PSX | `.chd`, `.bin`+`.cue`, `.iso` | `SYSTEM.CNF` | |
| PS2 | `.chd`, `.iso` | `SYSTEM.CNF` | |
| Switch | `.nsp`, `.xci` | NCA header / cnmt | Encrypted NCA needs `prod.keys` |
| 3DS | `.3ds`, `.cci`, `.z3ds`, `.zcci` | NCSD header | |
| Wii | `.rvz`, `.iso` | Boot header | |
| Wii U | `.wua` | Disc header | |
| PS3 | `.sfo` (extracted) | PARAM.SFO | |
| Xbox 360 | `.xex` | XEX header | |

## Building sigil

```sh
git clone --recursive https://github.com/rommforge/argosy-sigil.git
cd argosy-sigil
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
# produces: sigil (CLI) and libsigil.a / sigil.lib
```

All platform features are ON by default. Disable individual ones with `-DSIGIL_WITH_CHD=OFF` etc. if you don't need them.

### Windows (MSVC)

argosy-sigil uses POSIX headers (`dirent.h`, `unistd.h`, `off_t`) not available in MSVC. Two options:

1. **MinGW-w64** — native POSIX support, no shims needed
2. **MSVC + POSIX shim** — force-include a compatibility header providing `off_t`, `dirent`, `fseeko`/`ftello`, etc. See `examples/sigil_ppsspp_demo.c` build instructions for the exact `/FI` and `/I` flags.

## CLI Usage

Extract the save_id from any supported ROM:

```sh
# Auto-detect platform from extension (works for .cso, .nsp, .3ds, etc.)
sigil /path/to/game.nsp

# Force platform (needed for ambiguous extensions like .chd, .iso, .bin)
sigil --platform=psp /path/to/game.chd
sigil --platform=psx /path/to/game.bin
sigil --platform=ps2 /path/to/game.iso

# Switch NCA decryption (encrypted ROMs)
sigil --prod-keys=/path/to/prod.keys --platform=switch /path/to/game.nsp
```

Output format:

```
platform=psp title_id=ULUS10167 raw_serial=ULUS-10167 save_id=ULUS10167 usage=folder-prefix source=binary experimental=0
```

Fields:
- **platform** — detected/forced platform slug
- **title_id** — canonical 9-char game ID (e.g. `ULUS10167`)
- **raw_serial** — dashed form from disc (`ULUS-10167`)
- **save_id** — folder/file prefix for saves (same as title_id for PSP)
- **usage** — how saves are organized (`folder-prefix`, `folder-exact`, `file-exact`)
- **source** — `binary` (parsed from disc structure) or `filename` (fallback regex)
- **experimental** — 1 if the extractor path is unverified against real samples

## C API Usage

Link against `libsigil.a` (or `sigil.lib` on Windows):

```c
#include "sigil.h"

sigil_result result;
sigil_options opts = {
    .struct_version = SIGIL_OPTIONS_V1,
    .flags = SIGIL_FLAG_FILENAME_FALLBACK
};

int rc = sigil_extract_from_path(
    "F:\\ROMS\\psp\\Marvel - Ultimate Alliance (USA) (v2.00).chd",
    SIGIL_PLATFORM_PSP,   /* or SIGIL_PLATFORM_AUTO for extension-based detection */
    &opts, &result);

if (rc == SIGIL_OK) {
    printf("save_id: %s\n", result.save_id);       /* "ULUS10167" */
    printf("source:  %s\n",                        /* "binary" */
           result.source == SIGIL_SOURCE_BINARY ? "binary" : "filename");
    printf("experimental: %d\n", result.experimental);  /* 0 */
}
```

### Auto-detect platform

Pass `SIGIL_PLATFORM_AUTO` to let sigil sniff from the file extension:

```c
sigil_extract_from_path("/path/to/game.nsp", SIGIL_PLATFORM_AUTO, &opts, &result);
/* result.platform == SIGIL_PLATFORM_SWITCH */
```

Note: `.chd`, `.iso`, and `.bin` are ambiguous (used by PSP/PSX/PS2/GC/Wii). For these, force the platform with `--platform` or `SIGIL_PLATFORM_PSP` etc.

### Reading the result

```c
sigil_result r;
sigil_extract_from_path(rom, SIGIL_PLATFORM_PSP, NULL, &r);

/* All fields populated on success: */
r.title_id;        /* "ULUS10167"         — canonical game ID */
r.raw_serial;      /* "ULUS-10167"        — dashed form from UMD_DATA.BIN */
r.save_id;         /* "ULUS10167"         — prefix for save directories/files */
r.platform;        /* SIGIL_PLATFORM_PSP  — enum value */
r.source;          /* SIGIL_SOURCE_BINARY — binary or filename fallback */
r.usage;           /* SIGIL_USAGE_FOLDER_PREFIX — save organization type */
r.experimental;    /* 0 — verified against real samples */
```

## End-to-End Example: ROM → Save Snapshot

This is the full pipeline: extract save_id from a ROM, scan PPSSPP's save directory, register each matching folder, and create a versioned snapshot.

```c
#include "sigil.h"
#include "savesync.h"

int main(void) {
    /* 1. Extract save_id from ROM */
    sigil_result sig;
    sigil_extract_from_path(
        "F:\\ROMS\\psp\\Marvel - Ultimate Alliance (USA) (v2.00).chd",
        SIGIL_PLATFORM_PSP, NULL, &sig);

    if (sig.experimental) return 1;  /* safety check */

    /* 2. Initialize libsavesync */
    sv_init("./savesync_db");

    /* 3. Load PPSSPP manifest */
    sv_manifest_t *manifest = sv_manifest_create();
    sv_manifest_load("manifests/ppsspp.cfg", manifest);

    /* 4. Register save folders matching the prefix */
    /* (In practice, scan SAVEDATA/ for dirs starting with sig.save_id) */
    sv_register_opts_t opts = {
        .live_path = "F:\\EMULATORS\\ppsspp\\memstick\\PSP\\SAVEDATA\\ULUS10167Game00",
        .platform  = "psp",
        .emulator  = "ppsspp",
        .game_id   = sig.save_id,
        .shape     = SV_SHAPE_DIRECTORY,
    };

    sv_status_t st;
    sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);

    /* 5. Create versioned snapshot */
    sv_save_result_t save_res;
    sv_save(reg, NULL, &save_res);

    /* 6. Verify */
    sv_id_t entry_ids[16];
    size_t count;
    sv_list_entries(/* reg_id */ ..., entry_ids, 16, &count);

    sv_unregister(reg);
    sv_manifest_free(manifest);
    sv_shutdown();
    return 0;
}
```

See `examples/sigil_ppsspp_demo.c` for the complete, runnable version with CLI argument parsing and directory scanning.

## .chd and PSP — What You Need to Know

PSP saves distributed as `.chd` files are unusual but valid. Here's what happens internally:

1. Sigil detects `--platform=psp` and calls `sigil_io_open_chd()` (`src/io_chd.c`)
2. libchdr opens the CHD as a CD image (2048-byte user-data sectors)
3. The PSP extractor reads `UMD_DATA.BIN` from the ISO9660 filesystem inside
4. `UMD_DATA.BIN` contains `ULUS-10167|...` — parsed to extract the 9-char game ID

**The `.cso v2/LZ4` caveat does not apply to `.chd`.** That caveat is specific to the CSO path (`io_cso.c`), which only supports CSO v1 (raw zlib deflate). CHD uses its own codec stack (zlib/lzma/zstd) handled entirely by libchdr.

**The `.chd` extension caused no problems** in real-world testing with `Marvel - Ultimate Alliance (USA) (v2.00).chd`.

## Caveats

| Issue | Affected | Mitigation |
|-------|----------|------------|
| CSO v1 only | `.cso`/`.ciso` PSP files | v2 LZ4 CSOs silently fail; no error — check `experimental` flag |
| CHD ambiguous extension | `.chd` files | Force `--platform=psp` (or psx/ps2); auto-detect returns `AUTO` |
| Encrypted Switch NCA | `.nsp` with encrypted entries | Provide `--prod-keys` or accept `source=filename` fallback |
| Windows MSVC build | argosy-sigil, libsavesync | Needs POSIX compat shim or MinGW-w64 |
