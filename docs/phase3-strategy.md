# Phase 3 — SV_MODE_STRATEGY: Design Spec

**Status:** Authoritative contract for implementation.  
**Scope:** Identity resolution + manifest-driven strategy mode on top of existing DEFAULT mode.

---

## 1. What a "Manifest" Is

A manifest is a per-platform/emulator configuration that tells the engine how to identify saves and where to find them. It is provided to `sv_register()` when `mode = SV_MODE_STRATEGY`.

### 1.1 Manifest Struct

```c
typedef struct {
    char platform[64];          /* e.g. "ps2", "psp", "snes" — required */
    char emulator[64];          /* e.g. "pcsx2", "ppsspp" — optional, NULL if unused */
    sv_save_shape_t shape;      /* save shape — required */
    
    /* Identity method */
    sv_identity_method_t identity_method;  /* how to identify the game */
    
    /* Tier 1: serial extraction params */
    char serial_file[256];      /* file to read within save dir, e.g. "SYSTEM.CNF" */
    size_t serial_offset;       /* byte offset to start reading */
    size_t serial_length;       /* bytes to read for pattern matching */
    char serial_pattern[256];   /* regex-like pattern to extract serial, e.g. "BOOT2 = {SERIAL}" */
    
    /* Tier 2: checksum params */
    size_t checksum_offset;     /* byte offset of checksum in file */
    size_t checksum_size;       /* size of checksum field (2 or 4 bytes) */
    bool checksum_big_endian;   /* byte order of checksum */
    
    /* Save path template */
    char save_path_template[4096]; /* with {game_id}, {platform}, {emulator} placeholders */
} sv_manifest_t;
```

### 1.2 Identity Method Enum

```c
typedef enum {
    SV_IDENTITY_NONE = 0,       /* no identity resolution */
    SV_IDENTITY_SERIAL_CNF,     /* SYSTEM.CNF-style (PS1/PS2) */
    SV_IDENTITY_SERIAL_SFO,     /* PARAM.SFO-style (PSP/PS2) */
    SV_IDENTITY_SERIAL_IPBIN,   /* IP.BIN-style (Saturn) */
    SV_IDENTITY_BOOT_HEADER,    /* Boot header (GameCube/Wii) */
    SV_IDENTITY_CHECKSUM,       /* Header checksum + file size (SNES/Genesis) */
    SV_IDENTITY_PLUGGABLE,      /* Caller-supplied hash-DB callback */
} sv_identity_method_t;
```

---

## 2. Identity Resolution: Step by Step

Given a save file/directory and a manifest, the engine identifies the game using the **single method** selected by `identity_method`. The method is chosen at manifest/registration time based on the platform and format — it is NOT a runtime cascade that tries multiple methods.

### Step 1: Determine identity method
- If `manifest->identity_method == SV_IDENTITY_NONE`, skip resolution (game_id stays NULL).
- If `manifest->identity_method == SV_IDENTITY_PLUGGABLE`, invoke the caller's hash-DB callback (see §4).
- Otherwise, proceed to Step 2 for the selected method.

### Step 2: Read the target file
- For `SV_IDENTITY_SERIAL_CNF` / `SV_IDENTITY_SERIAL_SFO` / `SV_IDENTITY_SERIAL_IPBIN` / `SV_IDENTITY_BOOT_HEADER`:
  - If `live_path` is a file, read it directly.
  - If `live_path` is a directory, look for `manifest->serial_file` within it.
  - If the file doesn't exist or is too short, resolution fails (game_id stays NULL).

### Step 3: Extract and match
- Read `serial_length` bytes at `serial_offset` from the target file.
- Apply `serial_pattern`: `{SERIAL}` is replaced by the read bytes, then matched against the pattern.
- If the pattern contains literal characters (e.g., `BOOT2 = `), they must match exactly.
- If a capture group is specified (e.g., `{SERIAL:8}`), extract that many characters as the game serial.
- The extracted serial becomes the `game_id`.

### Step 4: Checksum method (Tier 2)
- If `identity_method == SV_IDENTITY_CHECKSUM`:
  - Read the checksum at `checksum_offset` (2 or 4 bytes, big/little endian per config).
  - Return a formatted key string (`CKSUM_XXXXXXXX_XXXXXXXX`).
  - This is NOT a fallback from Tier 1 — it is the selected method.

---

## 3. How STRATEGY Mode Interacts with Existing Calls

### sv_register()
- **DEFAULT mode**: No identity resolution. `game_id` is whatever the caller supplies (or NULL).
- **STRATEGY mode**: If manifest is provided, auto-detect `game_id` using the tiered resolution above. Caller-supplied `game_id` is overridden by detected value. If detection fails, `game_id` remains NULL (non-fatal).

### sv_save() / sv_pull()
- **No behavioral change** from DEFAULT mode. The manifest influences identity and path resolution at registration time, not at save/pull time. Atomic copy-out/in, dedup, retention all work identically.

### sv_update_register()
- If manifest changes (e.g., emulator version updated), caller can re-register with new manifest. Identity resolution runs again.

---

## 4. Pluggable Hash-DB Callback (Tier 3)

```c
/* Callback signature for hash-DB lookup */
typedef sv_status_t (*sv_hash_db_lookup_fn)(
    const uint8_t *hash,       /* content hash to look up */
    size_t hash_len,            /* hash length (20 for SHA-1) */
    const char *platform,       /* platform hint */
    char *out_game_id,          /* buffer for game_id result */
    size_t out_game_id_len,     /* buffer size */
    void *user_ctx              /* caller context */
);

/* Set on manifest before registration */
manifest->hash_db_lookup = my_lookup_fn;
manifest->hash_db_ctx = my_context;
```

If `identity_method == SV_IDENTITY_PLUGGABLE` and the callback is set, it is invoked as the selected identification method. If the callback returns `SV_OK`, the returned `out_game_id` is used. If it returns `SV_ERR_NOT_FOUND` or any error, resolution fails silently (game_id stays NULL).

---

## 5. Edge Cases

| Case | Behavior |
|------|----------|
| **No manifest provided** | `sv_register()` works in DEFAULT mode even if `mode = SV_MODE_STRATEGY` — falls back to no identity resolution. Non-fatal. |
| **Manifest file missing/unreadable** | `sv_manifest_load()` returns `SV_ERR_NOT_FOUND`. Registration proceeds without manifest. |
| **Save file too short for header** | Identity resolution fails. `game_id` stays NULL. Non-fatal. |
| **Header malformed / pattern doesn't match** | Identity resolution fails. `game_id` stays NULL. Non-fatal. |
| **No hash-DB entry found (Tier 3)** | Callback returns `SV_ERR_NOT_FOUND`. `game_id` stays NULL. Non-fatal. |
| **Ambiguous match (two candidates tie)** | For Tier 1/2: return the first match (deterministic). For Tier 3: caller's callback decides. |
| **manifest->serial_file not found in directory** | Identity resolution fails for Tier 1. `game_id` stays NULL. |
| **manifest->shape differs from actual file type** | No runtime check — caller is responsible for correctness. Shape affects copy behavior only. |

---

## 6. New Public API Functions

```c
/* Manifest lifecycle */
sv_manifest_t *sv_manifest_create(void);
void           sv_manifest_free(sv_manifest_t *manifest);

/* Load manifest from text file */
sv_status_t sv_manifest_load(const char *path, sv_manifest_t *out_manifest);

/* Save manifest to text file */
sv_status_t sv_manifest_save(const char *path, const sv_manifest_t *manifest);

/* Register with manifest (STRATEGY mode) */
sv_registration_t *sv_register_with_manifest(
    const sv_register_opts_t *opts,
    const sv_manifest_t *manifest,
    sv_status_t *out_status
);
```

---

## 7. Manifest Text File Format

Simple key-value format, one field per line:

```
platform=ps2
emulator=pcsx2
shape=file
identity=serial_cnf
serial_file=SYSTEM.CNF
serial_offset=0
serial_length=256
serial_pattern=BOOT2 = {SERIAL:12}
save_path_template={live_path}
```

Lines starting with `#` are comments. Empty lines are ignored.

---

## 8. Implementation Order

1. Add `sv_identity_method_t` and manifest struct to `savesync.h`
2. Add manifest lifecycle functions (`create`, `free`, `load`, `save`)
3. Add `sv_register_with_manifest()` 
4. Implement serial extraction for `SV_IDENTITY_SERIAL_CNF` (PS1/PS2 SYSTEM.CNF)
5. Implement serial extraction for `SV_IDENTITY_SERIAL_SFO` (PSP/PS2 PARAM.SFO)
6. Implement checksum matching for `SV_IDENTITY_CHECKSUM`
7. Wire pluggable hash-DB callback
8. Update metadata store to persist `game_id` from identity resolution
