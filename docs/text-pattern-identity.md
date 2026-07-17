# SV_IDENTITY_TEXT_PATTERN — Design Note

**Status:** Approved for implementation.
**Scope:** Minimal pattern-based text extraction from files. NOT a regex engine.

---

## 1. New Enum Value

```c
SV_IDENTITY_TEXT_PATTERN = 8  /* appended after SV_IDENTITY_ROM_HEADER (7) */
```

## 2. Pattern Syntax (deliberately minimal)

The pattern is matched against the entire file content (or a substring starting at `text_pattern_offset`).

| Syntax | Meaning | Example |
|--------|---------|---------|
| Literal text | Must match exactly | `BOOT2 = cdrom0:\` |
| `{ID:N}` | Capture exactly N characters | `{ID:11}` captures 11 chars |
| `{ID:*}` | Capture until the next character in the pattern | `{ID:*};` captures until `;` |
| `_` | Match any single character (skip wildcard) | `XX_XX` matches `XY0X` |

**Why minimal pattern beats regex:**
- libsavesync is a dependency-free pure C library. Linking PCRE or POSIX regex adds a platform-dependent dependency (different regex implementations on Linux vs macOS vs Windows).
- The patterns needed for real-world save identification are simple: literal prefixes + fixed-length captures + a few wildcards. Full regex is overkill.
- A minimal pattern is easy to audit, test, and reason about — no catastrophic backtracking, no regex injection, no "write-only" patterns.

## 3. New Manifest Fields

```c
char text_pattern[1024];       /* the pattern string */
char text_pattern_source[256]; /* file to scan, relative to live_path (e.g. "SYSTEM.CNF") */
size_t text_pattern_offset;    /* byte offset to start reading (0 = start of file) */
size_t text_pattern_length;    /* bytes to read (0 = entire file) */
char normalize_strip[64];     /* characters to strip from extracted ID */
char normalize_replace[256];  /* "XY" means replace X with Y (e.g. "_-") */
```

All default to zero/empty (zero-default rule).

## 4. How It Works

1. Open the file at `{live_path}/{text_pattern_source}` (or `{live_path}` if source is empty)
2. Read `text_pattern_length` bytes starting at `text_pattern_offset` (or entire file if length is 0)
3. Scan the content for the pattern:
   - Literal characters must match exactly
   - `{ID:N}` captures N characters
   - `{ID:*}` captures until the delimiter character (the char immediately after `*` in the pattern)
   - `_` matches any single character
4. Apply normalization: strip chars in `normalize_strip`, replace pairs in `normalize_replace`
5. Return the extracted ID as `game_id`

## 5. PCSX2 SYSTEM.CNF Example

Pattern: `BOOT2 = cdrom0:\{ID:11};1`
Content: `BOOT2 = cdrom0:\TEST_000.01;1`

Match:
- `BOOT2 = cdrom0:\` — literal prefix (16 chars), matches
- `{ID:11}` — capture 11 chars: `TEST_000.01`
- `;1` — literal suffix, matches

Extracted: `TEST_000.01`
Normalization: `normalize_strip=.;` + `normalize_replace=_:-` → `TEST-00001`

## 6. Error/Edge Cases

| Case | Behavior |
|------|----------|
| `text_pattern` is empty | `SV_ERR_INVALID_ARG` → game_id stays NULL |
| Source file not found | `SV_ERR_NOT_FOUND` → game_id stays NULL |
| File shorter than offset+length | `SV_ERR_NOT_FOUND` → game_id stays NULL |
| Pattern doesn't match content | `SV_ERR_NOT_FOUND` → game_id stays NULL |
| Empty capture `{ID:0}` or `{ID:*}` with no delimiter | `SV_ERR_INVALID_ARG` → game_id stays NULL |

## 7. Manifest Fields Summary

```
text_pattern=BOOT2 = cdrom0:\{ID:11};1
text_pattern_source=SYSTEM.CNF
text_pattern_offset=0
text_pattern_length=0
normalize_strip=.;
normalize_replace=_:-
```
