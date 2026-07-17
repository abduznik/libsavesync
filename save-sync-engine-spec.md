# Save Sync Engine — Project Specification

**Working name:** `libsavesync` (rename later)
**Status:** Design-complete, pre-implementation
**Author context:** Designed as a companion/successor concept to [Freegosy](https://github.com/abduznik/Freegosy), extracting its proven per-emulator save logic into a standalone, embeddable core.

---

## 1. Philosophy

### 1.1 What this is

A **pure C library** (compiled as both a linkable static/shared library with a stable C ABI, *and* a standalone binary with an IPC protocol) that manages the precise lifecycle of emulator/game save files: identifying exactly which save belongs to which game, safely versioning it locally, and optionally handing it off to external transports for cross-device sync.

It is **not**:
- An emulator or launcher
- A cloud service
- A process watchdog
- A save-sync orchestration server (like RomM's backend)

It **is**:
- A local, passive, embeddable save lifecycle manager
- Infrastructure other apps link against or shell out to — the "libgit2 of save sync"

### 1.2 Why this is different from what exists

Researched prior art before committing to this design:

| Project | What it does | Why it's not this |
|---|---|---|
| Syncthing / cloud-mounted folders | Generic folder sync | Zero game-awareness, fuzzy matching, no identity layer |
| EmuSync, SaveSync, CrossSave-Cloud | Hobbyist folder/extension-based save backup | Extension-guessing, no precision identity, monolithic apps |
| RomM's save sync engine (4.9+) | Server-orchestrated multi-device sync, baseline-anchored conflict resolution | Sophisticated, but logic lives in the server backend — not embeddable, not reusable by other apps |
| Ludusavi | Cross-platform save backup, manifest-driven | Excellent prior art for "detection as a library, delegate transport out" — but Rust/CLI-GUI, not a C-ABI embeddable core, and PC-game-store focused rather than emulator-precision focused |
| Freegosy (this author's own app) | Already has precise per-emulator save strategies shipped (PCSX2 serial folders, Dolphin `.gci` extraction, PSP save-data dirs, Switch title-ID mapping, launch grace windows, per-device isolation) | Logic is embedded inside one Flutter app, not extractable/reusable by anyone else |

**The gap this fills:** nobody has taken proven per-emulator save precision logic and shipped it as a **standalone, embeddable, C-ABI library** that any app (Flutter via FFI, a Decky plugin, a C# app, a homebrew tool) can link against without reimplementing save-location logic themselves. This is a generalization/extraction project, not a from-scratch gamble — Freegosy's existing strategies are the spec to port from.

### 1.3 Core design principles (apply to every layer)

1. **Passive, never a watchdog.** The library never inspects processes, never decides *when* to act on its own. Every operation is explicitly triggered by the caller. Process-lifecycle detection is the caller's problem, not this library's — cross-platform process introspection is a different, riskier class of problem (permissions, PID races, security software false-positives) that doesn't belong inside a save-integrity library.
2. **Never touch the live file unsafely.** All reads of the live save happen via atomic copy-out into a versioned "magazine" entry. All writes to the live save happen via atomic replace (temp file + rename), never in-place overwrite. The live path is only ever touched at two chokepoints: copy-out (`save`) and copy-in (`pull`/`pull_select`).
3. **Nothing destructive without an explicit choice.** Conflicts are always reported by default, never silently resolved. Every function that could clobber existing data takes an explicit `on_conflict` policy; the default is always the safe, non-destructive option.
4. **Every field is optional, zero-default.** No field is ever required unless it's structurally necessary (e.g. `live_path`). Unset fields are zeroed/NULL and treated as "unknown," never as an error. This is both a usability rule (users can do a bare 1:1 sync with zero metadata) and an ABI-stability rule (new fields can be added later without breaking old callers, since old callers simply leave them zeroed).
5. **Plain C ABI, opaque structs at the boundary.** No struct is ever handed across the API boundary by value for callers to allocate — only opaque handles + accessor functions, or read-only output snapshots. This is standard practice for ABI-stable C libraries (confirmed via research: C's minimal binary surface is *why* it's used as the stable boundary even for C++/Rust projects). Never reorder or remove struct fields once shipped.
6. **Everything is entry-first; registration is just a pointer.** A magazine entry ("child") belongs to a registration ("parent") by reference, not by hard binding. Parents can be deleted, leaving orphaned entries; entries can be explicitly re-parented. Nothing is silently deleted as a side effect of an unrelated operation.
7. **Networking-free core, pluggable transport.** The core never contains socket/HTTP code. Cross-device sync is delegated entirely to caller-supplied function pointers (a `push`/`pull` contract), so USB, HTTP, LAN, or terminal-based transports can all be implemented externally without touching library source — same pattern as `libcurl`/`libgit2`/`rclone`.
8. **Identity without a heavy database.** Game identity resolution is tiered: embedded serial extraction first (PSP/PS1/PS2 `SYSTEM.CNF`/`PARAM.SFO`, Saturn `IP.BIN`, GameCube/Wii boot headers — zero external data needed), header checksums second (SNES/Genesis internal checksum + file size), and an optional pluggable hash-database module last, for the remaining edge cases only. The core library ships database-free.
9. **A default mode must exist for testing without any real strategy data.** `SV_MODE_DEFAULT` allows registering and exercising the full pipeline (register → save → pull → rotate) against any arbitrary file or folder, with zero platform/emulator-specific logic. This is what makes the library testable on day one with fake files, before a single real emulator strategy is implemented.

---

## 2. The Six Layers

### Layer 1 — Interface
Dual entry points: a linkable C ABI (static/shared library) and a standalone binary exposing the same operations over an IPC protocol (for callers that can't easily FFI, e.g. some JS runtimes). Both are thin wrappers over the same internal core — this is a communication layer, not a logic duplication. ABI stability rules (opaque structs, append-only fields, no reordering) apply from the very first shipped version.

### Layer 2 — Registration / Identity
`sv_register()` creates a new registration from a `live_save_path` (required) plus optional `rom_path`, `platform`, `emulator`, `product_version`, and mode (`SV_MODE_DEFAULT` or `SV_MODE_STRATEGY`). Produces an opaque handle and a persistent 8-character unique ID. In `STRATEGY` mode, identity resolution (Layer 2's tiered serial/checksum/DB approach) and manifest-driven path/shape strategy apply; in `DEFAULT` mode, none of that runs — it's a bare, generic file/folder tracker.

`sv_update_register()` mutates an existing registration (e.g. relocating `live_save_path`). Uses an explicit `set_mask` bitfield to distinguish "leave unchanged" from "explicitly reset to default/empty," since update semantics can't rely on zero-default alone. Includes a "poke check" against the destination path: reports `SV_CONFLICT_EXISTING_DATA` by default rather than guessing; caller can pass `SV_ON_CONFLICT_OVERRIDE` or `SV_ON_CONFLICT_ABORT_SILENT` explicitly. Relocation supports `SV_RELOCATE_COPY` (default, non-destructive) or `SV_RELOCATE_MOVE` (copy-verify-then-delete-source, never delete-then-copy).

`sv_unregister()` drops the metadata record only — it never touches the magazine or live files. Entries under a removed registration become **orphans** (`parent_id` zeroed), not deleted.

### Layer 3 — Metadata Store
A small local file/record store, one entry per registration and one per magazine entry. Carries a **file-level `schema_version`** (separate from per-record fields) so future field additions can be migrated rather than breaking existing stores. Designed to stay tiny — this is metadata, not save content.

**Full per-entry/registration field set (all optional except `live_path`/`shape`):**

```c
/* ---- identity ---- */
const char *game_id;          /* NULL if unknown */
const char *rom_path;         /* NULL if unknown */
uint8_t     rom_hash[20];     /* zeroed if not computed */
bool        rom_hash_set;

/* ---- context ---- */
const char *platform;         /* NULL if unknown */
const char *emulator;         /* NULL if unknown */
const char *product_version;  /* NULL if unknown — emulator/core/backend version, caller's choice of meaning */

/* ---- save content ---- */
const char     *live_path;    /* required */
sv_save_shape_t shape;        /* FILE / DIRECTORY / CONTAINER / ARCHIVE / UNKNOWN (opaque-blob fallback) */

/* ---- integrity & versioning ---- */
uint8_t  content_hash[20];    /* zeroed if not computed — "did it change" */
bool     content_hash_set;
bool     integrity_ok;        /* sanity-checked at copy-out time — "is it corrupt" */

/* ---- disambiguation metadata ---- */
int64_t  mtime;                /* 0 if not captured */
uint64_t size_bytes;           /* 0 if not captured */
uint32_t playtime_seconds;     /* 0 if not supplied */
const char *label;             /* optional human note, NULL if unset */

/* ---- magazine bookkeeping ---- */
sv_id_t     entry_id;              /* 8-char unique ID */
sv_id_t     parent_id;             /* registration this entry currently belongs to; zeroed if orphaned */
const char *magazine_slot_path;    /* read-only, detached/sandboxed — NULL until copy-out has happened */
```

### Layer 4 — Local Save / Pull (magazine-safe)

- **`sv_save(reg, opts, &out_result)`** — atomic copy-out from `live_path` into a **new** magazine entry (never overwrites an existing entry). Computes `content_hash` + `integrity_ok`. Default behavior: skip creating a new entry if the hash matches the most recent one (avoids pointless duplication/retention churn). `opts.force = true` overrides this — always creates a new entry regardless of hash match, for UI "force push, make me feel safe" buttons. Force still respects the retention cap (see Layer 5) — it bypasses dedup, not rotation.
- **`sv_pull(reg, opts, &out_result)`** — atomic copy-in of the *most recent* magazine entry to `live_path`. Conflict-checked: if the live file changed since the last known sync point, default is report-don't-clobber (`SV_PULL_ON_CONFLICT_REPORT`); explicit override required to proceed. Even on override, the about-to-be-overwritten live data is itself snapshotted first — overrides are never truly destructive, only require an explicit choice.
- **`sv_pull_select(reg, entry_id, opts, &out_result)`** — identical mechanics to `pull()`, but targets a specific entry instead of "latest." Pairs with `sv_list_entries()` for building a version picker UI.
- **`sv_list_registrations()` / `sv_read_registration(id)`** — enumerate and inspect registrations.
- **`sv_list_entries(reg_id | NULL)` / `sv_read_entry(entry_id)`** — enumerate and inspect magazine entries; passing a null/sentinel `reg_id` lists **orphaned** entries (no living parent).
- **`sv_reparent_entry(entry_id, new_parent_reg_id)`** — explicitly re-links an entry to a different (or recovered) registration. This is the "the father can change" mechanism — e.g. a ROM was re-dumped/re-identified, or a user reclaims an orphan.
- **`sv_delete_entry(entry_id)`** — the *only* manual hard-delete path, used for cleaning up orphans. Never invoked implicitly by any other function.

### Layer 5 — Retention / Rotation

Runs automatically as the final step of every successful `sv_save()` (normal or forced) — never triggered by any other function. FIFO eviction: oldest entry under that specific registration is deleted once `retention_count` is exceeded. `retention_count == 0` means **keep-forever**, not "delete immediately" (consistent with the zero-default rule — zero is never destructive). Orphaned entries are outside any registration's cap and are never auto-evicted; they persist until manually reparented or explicitly deleted via `sv_delete_entry`. Rotation results are folded into `sv_save_result_t` so callers learn what was evicted without a separate call.

### Layer 6 — External Push / Pull (Pluggable Transport)

The core stays networking-free. A transport is a struct of two nullable function pointers:

```c
typedef sv_xport_status_t (*sv_xport_push_fn)(
    const sv_entry_info_t *entry,
    const char *magazine_slot_path,   /* read-only source — never the live path */
    void *user_ctx
);

typedef sv_xport_status_t (*sv_xport_pull_fn)(
    const sv_id_t reg_id,
    sv_entry_info_t *out_entry,
    char *out_dest_buf, size_t out_dest_buf_len,
    void *user_ctx
);
```

`sv_push_external(entry_id, transport)` sends one specific magazine entry out. `sv_pull_external(reg_id, transport, pull_opts)` fetches a remote save *into a new local magazine entry* — it never writes directly to the live path; getting a save onto the device and promoting it to live are always two separate, explicit steps (`pull_external` then `pull`). This mirrors `sv_save()`'s mechanics (new entry, hash computed, retention triggered) and reuses the same conflict-handling struct as local `pull()` — one conflict model for the whole library. Transports can be write-only, read-only, or both. Errors are structured (`SV_XPORT_ERR_AUTH`, `_CONFLICT`, `_NOT_FOUND`, `_IO`, `_OTHER`) without the core knowing anything about the underlying protocol.

---

## 3. Data Types Reference

```c
typedef char sv_id_t[9]; /* 8-char unique ID + null terminator */

typedef struct sv_registration sv_registration_t; /* opaque handle */

typedef enum {
    SV_MODE_DEFAULT = 0,
    SV_MODE_STRATEGY
} sv_registration_mode_t;

typedef enum {
    SAVE_SHAPE_UNKNOWN = 0,
    SAVE_SHAPE_FILE,
    SAVE_SHAPE_DIRECTORY,
    SAVE_SHAPE_CONTAINER,
    SAVE_SHAPE_ARCHIVE
} sv_save_shape_t;

typedef enum {
    SV_CONFLICT_NONE = 0,
    SV_CONFLICT_EXISTING_DATA
} sv_conflict_t;

typedef enum {
    SV_ON_CONFLICT_REPORT = 0,
    SV_ON_CONFLICT_OVERRIDE,
    SV_ON_CONFLICT_ABORT_SILENT
} sv_on_conflict_t;

typedef enum {
    SV_RELOCATE_COPY = 0,
    SV_RELOCATE_MOVE
} sv_relocate_mode_t;

typedef enum {
    SV_PULL_ON_CONFLICT_REPORT = 0,
    SV_PULL_ON_CONFLICT_OVERRIDE
} sv_pull_on_conflict_t;

typedef enum {
    SV_EVICT_OLDEST = 0,
    SV_EVICT_NONE
} sv_eviction_policy_t;

typedef enum {
    SV_XPORT_OK = 0,
    SV_XPORT_ERR_IO,
    SV_XPORT_ERR_AUTH,
    SV_XPORT_ERR_NOT_FOUND,
    SV_XPORT_ERR_CONFLICT,
    SV_XPORT_ERR_OTHER
} sv_xport_status_t;
```

---

## 4. Function Signatures Reference

```c
/* Layer 2 — Registration */
sv_registration_t *sv_register(const sv_register_opts_t *opts, sv_status_t *out_status);
sv_status_t         sv_update_register(sv_registration_t *reg, const sv_update_opts_t *opts, sv_update_result_t *out_result);
void                sv_unregister(sv_registration_t *reg);
sv_status_t         sv_list_registrations(sv_id_t *out_ids, size_t max_ids, size_t *out_count);
sv_status_t         sv_read_registration(const sv_id_t id, sv_registration_info_t *out_info);

/* Layer 4 — Local save/pull */
sv_status_t sv_save(sv_registration_t *reg, const sv_save_opts_t *opts, sv_save_result_t *out_result);
sv_status_t sv_pull(sv_registration_t *reg, const sv_pull_opts_t *opts, sv_pull_result_t *out_result);
sv_status_t sv_pull_select(sv_registration_t *reg, const sv_id_t entry_id, const sv_pull_opts_t *opts, sv_pull_result_t *out_result);
sv_status_t sv_list_entries(const sv_id_t reg_id /* or NULL for orphans */, sv_id_t *out_ids, size_t max_ids, size_t *out_count);
sv_status_t sv_read_entry(const sv_id_t entry_id, sv_entry_info_t *out_info);
sv_status_t sv_reparent_entry(const sv_id_t entry_id, const sv_id_t new_parent_reg_id);
sv_status_t sv_delete_entry(const sv_id_t entry_id);

/* Layer 6 — External transport */
sv_status_t sv_push_external(const sv_id_t entry_id, const sv_transport_t *transport);
sv_status_t sv_pull_external(const sv_id_t reg_id, const sv_transport_t *transport, sv_pull_opts_t *pull_opts);
```

*(Full option/result struct definitions are inline in Section 2 above and were iteratively refined through the design conversation — treat Section 2 as the authoritative field-level spec, this section as the call-surface index.)*

---

## 5. Open / Deferred Items

These were explicitly scoped out or left for implementation-time judgment, not oversights:

- **Manifest schema for `SV_MODE_STRATEGY`** (per-platform/emulator path templates, identity method selection, save-shape declaration) — designed conceptually (Section 1.3, point 8) but not yet reduced to a concrete file format. Needed before real emulator strategies (RetroArch, PPSSPP, Dolphin, etc.) can be ported from Freegosy.
- **Directory/archive sub-file granularity** — whether `SAVE_SHAPE_DIRECTORY`/`ARCHIVE` entries need per-file hash lists for finer diffing, or whether whole-container hashing is sufficient for v1. Deferred as a v2 refinement.
- **Metadata store's actual on-disk format** (flat file, SQLite, key-value store) — deliberately left as an implementation detail, not a spec decision, since it doesn't affect the public API.
- **Pluggable identity-hash-database module** (tier 3 of Layer 2's identity resolution) — designed as an optional, separate, pluggable component so the core stays database-free; concrete DAT-format support deferred.

---

## 6. Suggested First Test Plan

1. Implement Layers 1, 3, 4, 5 fully; implement Layer 2 with **`SV_MODE_DEFAULT` only** (no identity resolution, no manifest).
2. Register a plain text file as a fake "save."
3. Exercise: `save()` → `list_entries()` → `read_entry()` → mutate the file → `save()` again → `list_entries()` (confirm 2 entries, dedup didn't fire since content changed) → `save()` unchanged (confirm dedup skip) → `save(force=true)` (confirm new entry despite identical hash) → `pull_select()` an older entry → confirm atomic replace worked → push retention count low (e.g. 2) and confirm oldest eviction fires correctly.
4. Repeat with a fake directory (`SAVE_SHAPE_DIRECTORY`) and a fake multi-file "container" to validate shape handling before any real emulator format is involved.
5. Only after the above is solid: implement one real `SV_MODE_STRATEGY` case (e.g. RetroArch single-file `.srm`, the simplest 99%-of-cases strategy) as the first real-world validation, using Freegosy's existing logic as reference.
6. Layer 6 (transport) can be tested with a trivial "copy to another local folder" reference transport before any real network transport is written.

This order validates the hardest-to-get-right parts (atomicity, conflict handling, retention, ABI shape) against zero-risk fake data first, and defers emulator-specific complexity to last.
