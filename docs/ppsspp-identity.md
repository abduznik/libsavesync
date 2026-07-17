# PPSSPP Identity: Directory-Name Pattern

## Decision

**Chosen approach: SV_IDENTITY_TEXT_PATTERN with `text_pattern_source=_dirname`**

The game ID is the first 9 characters of each save directory's basename, extracted via the pattern `{ID:9}`.

## Rejected Alternatives

### PARAM.SFO (rom_header approach)
PARAM.SFO inside each save folder contains a `SAVEDATA_DIRECTORY` field, but its value is the *full* directory name including the suffix (e.g., `ULUS10509001`), not just the game ID. Since the suffix is already encoded in the directory name, reading PARAM.SFO provides no additional value for game identification.

Furthermore, PARAM.SFO is a variable-length key-value format (PSF v1.1) with header-indicated offsets for key and value tables. Fixed-offset reading via `SV_IDENTITY_ROM_HEADER` would be unreliable because the TITLE_ID field's offset varies across saves.

### Fuzzy title scoring (Freegosy's approach)
Freegosy reads PARAM.SFO's TITLE field (e.g., "METAL GEAR SOLID PEACE WALKER") and scores folders by matching game name words. This is a **display-name disambiguation step**, not the primary identification path. The directory-name pattern is the authoritative source and requires no fuzzy matching.

## Real-World Structure

PPSSPP save data lives in `PSP/SAVEDATA/` with directory names following a fixed convention:

```
<GameID><Suffix>
```

Where:
- **GameID** (9 chars): 4-letter region/publisher code + 5-digit game number
  - `ULUS10509` — US region
  - `ULES01372` — EU region
  - `ULJM05500` — JP region
- **Suffix** (variable length, 3-7 chars): save type identifier
  - `001`–`999`: save slots
  - `DAT`: game data
  - `DLC`, `DLCBGM`, `DLCTEX`, `DLCVOICE`: downloadable content
  - `SYSDATA`: system data

### Observed on this machine

Two games found at `~/Library/CloudStorage/GoogleDrive-.../Roms/psp/PSP/SAVEDATA/`:

| Directory | Game ID | Suffix |
|-----------|---------|--------|
| `ULUS10509001` | ULUS10509 | 001 |
| `ULUS10509DAT` | ULUS10509 | DAT |
| `ULUS10509DLC` | ULUS10509 | DLC |
| `ULUS10509DLCBGM` | ULUS10509 | DLCBGM |
| `ULUS10509DLCTEX` | ULUS10509 | DLCTEX |
| `ULUS10509DLCVOICE` | ULUS10509 | DLCVOICE |
| `ULES01372001` | ULES01372 | 001 |
| `ULES01372DAT` | ULES01372 | DAT |
| `ULES01372DLC` | ULES01372 | DLC |
| `ULES01372DLCBGM` | ULES01372 | DLCBGM |
| `ULES01372DLCTEX` | ULES01372 | DLCTEX |
| `ULES01372DLCVOICE` | ULES01372 | DLCVOICE |
| `ULUS10466SYSDATA` | ULUS10466 | SYSDATA |

All game IDs are exactly 9 characters. Suffixes are always at least 2 characters.

## Implementation

### `text_pattern_source=_dirname`

A special sentinel value for `text_pattern_source` that tells the pattern matcher to match against the **basename of `live_path`** instead of reading file content. This avoids inventing a new identity method — the existing `SV_IDENTITY_TEXT_PATTERN` machinery handles everything.

### Pattern: `{ID:9}`

Captures exactly 9 characters from the directory basename. No normalization needed — game IDs are already in canonical form (uppercase alphanumeric).

### Manifest: `ppsspp.cfg`

```ini
platform=psp
emulator=ppsspp
shape=directory
identity=text_pattern
text_pattern={ID:9}
text_pattern_source=_dirname
save_path_template={game_id}/{platform}
```

### Pattern parser fix

A trailing capture like `{ID:9}` at the end of a pattern previously failed because the while loop exited (`pi >= pat_len`) before the capturing block could execute. Fixed by:

1. Changing the loop condition to `while ((pi < pat_len || capturing) && ci < clen && !failed)`
2. Adding bounds guard: `char pc = (pi < pat_len) ? pat[pi] : '\0'`
3. Post-loop validation: rejecting incomplete fixed-length captures when content runs out

This fix is backward-compatible — existing patterns with literal text after captures (PCSX2, Ryujinx) continue to work unchanged.
