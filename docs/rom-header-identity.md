# SV_IDENTITY_ROM_HEADER — Design Note

**Status:** Approved for implementation.
**Scope:** Single new identity method for ROM header reading. No regex, no stem matching, no fuzzy scoring.

---

## 1. New Enum Value

```c
SV_IDENTITY_ROM_HEADER = 7  /* appended after SV_IDENTITY_PLUGGABLE (7) */
```

Appended to `sv_identity_method_t` — no reordering of existing values.

## 2. New Manifest Fields

```c
size_t rom_header_offset;   /* byte offset in ROM file to start reading */
size_t rom_header_length;   /* number of bytes to read (0 = use default per format) */
```

Both default to 0 (zero-default rule). When `identity_method == SV_IDENTITY_ROM_HEADER`:
- `rom_header_offset` is **required** (must be > 0 or registration fails)
- `rom_header_length` is optional (default: read 4 bytes — matches Dolphin GC, the primary use case)

## 3. How Read Bytes Become game_id

**Rule:** If all bytes are printable ASCII (0x20–0x7E), output as a raw ASCII string. Otherwise, output as a hex-encoded string (e.g. `"0x47414C45"`).

**Justification:** Dolphin GC uses 4-byte ASCII game IDs like `"TEST01"` (a synthetic test ID). Most other platforms with ROM-header IDs also use ASCII (PlayStation serials, NDS game codes). Hex encoding is the fallback for binary headers that aren't ASCII — this handles future use cases without requiring a separate method.

## 4. ROM File Path Source

**No API change needed.** `sv_register_opts_t` already has `rom_path` (line 101 of savesync.h). The caller supplies the ROM path at registration time. `sv_register_with_manifest()` already copies `opts->rom_path` into `reg->rom_path`. The new identity method reads from `reg->rom_path`.

When `rom_path` is NULL or empty, identity resolution fails with `SV_ERR_NOT_FOUND`.

## 5. Error/Edge Cases

| Case | Behavior |
|------|----------|
| `rom_path` is NULL or empty | `SV_ERR_NOT_FOUND` → game_id stays NULL |
| File not found at `rom_path` | `SV_ERR_NOT_FOUND` → game_id stays NULL |
| File shorter than `offset + length` | `SV_ERR_NOT_FOUND` → game_id stays NULL |
| `rom_header_offset` is 0 (not set) | `SV_ERR_INVALID_ARG` → game_id stays NULL |
| All extracted bytes are non-printable | Hex-encoded string (e.g. `"0x00FF"`) |
| Registration succeeds despite identity failure | Yes — identity failure is never fatal (consistent with all other methods) |

## 6. Dolphin GC Validation

GameCube ROM header game ID:
- **ISO format:** 4 ASCII bytes at offset `0x00`
- **RVZ format:** 4 ASCII bytes at offset `0x58`
- Example: `"TEST01"` = Synthetic test GameCube ID

Manifest fields:
```
identity=rom_header
rom_header_offset=0
rom_header_length=4
```

For RVZ format, caller would use `rom_header_offset=0x58`.
