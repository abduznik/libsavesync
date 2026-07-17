# RPCS3 Identity: Directory-Name Pattern

## Decision

**Chosen approach: SV_IDENTITY_TEXT_PATTERN with `text_pattern_source=_dirname`**

The game ID is the first 9 characters of each save directory's basename, extracted via the pattern `{ID:9}`. This is identical to the PPSSPP mechanism.

## Real-World Structure

RPCS3 save data lives in `dev_hdd0/home/00000001/savedata/` with directory names following a fixed convention:

```
<GameID><Suffix>
```

Where:
- **GameID** (9 chars): 4-letter region/publisher code + 5-digit game number
  - `BLUS30443` — US region
  - `BLUS30826` — US region
  - `BLES00676` — EU region
- **Suffix** (variable length, 1-20+ chars): save type or name
  - `SYSTEM`: system data
  - `F`, `L01`: profile/save slots
  - `DEMONSS005`, `-NAUGHTYBEARSAVEGAME`: compound save names
  - Some suffixes start with a hyphen separator

### Observed on this machine

Six save directories found at `~/Library/Application Support/rpcs3/dev_hdd0/home/00000001/savedata/`:

| Directory | Game ID | Suffix |
|-----------|---------|--------|
| `BLUS30443DEMONSS005` | BLUS30443 | DEMONSS005 |
| `BLUS30490SYSTEM` | BLUS30490 | SYSTEM |
| `BLUS30507-NAUGHTYBEARSAVEGAME` | BLUS30507 | -NAUGHTYBEARSAVEGAME |
| `BLUS30826F` | BLUS30826 | F |
| `BLUS30826L01` | BLUS30826 | L01 |
| `BLES00676-SAW` | BLES00676 | -SAW |

All game IDs are exactly 9 characters. Suffixes are always at least 1 character.

Additionally, 4 `.zip` backup files exist (2338.saves.zip, etc.) — these are RPCS3's automatic backup mechanism and contain the same directory structure.

### PARAM.SFO Confirmation

Each save directory contains a `PARAM.SFO` file with a `SAVEDATA_DIRECTORY` field that matches the full directory name (including suffix). This confirms the directory name as the authoritative identifier.

## Implementation

Same as PPSSPP: `text_pattern={ID:9}` with `text_pattern_source=_dirname`.

The pattern parser fix for trailing captures (made during PPSSPP implementation) enables this to work correctly.

## Comparison with PPSSPP

| Aspect | PPSSPP | RPCS3 |
|--------|--------|-------|
| Save path | `PSP/SAVEDATA/` | `dev_hdd0/home/00000001/savedata/` |
| Game ID format | 4 letters + 5 digits | 4 letters + 5 digits |
| Suffix examples | `001`, `DAT`, `DLCBGM` | `SYSTEM`, `F`, `L01`, `-SAW` |
| Suffix length | 3-7 chars | 1-20+ chars |
| PARAM.SFO present | Yes | Yes |
| `SAVEDATA_DIRECTORY` | Matches dir name | Matches dir name |
| Pattern needed | `{ID:9}` | `{ID:9}` |

The structure is identical for identification purposes — both use 9-char game IDs with variable-length suffixes.
