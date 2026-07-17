# Stem-Matching Identity — Design Proposal

**Status:** Decided — Option B adopted.
**Scope:** DuckStation (.mcd), mGBA (.sav/.srm), melonDS (.sav/.srm), RetroArch (.srm) — all use `{romStem}.{ext}` naming.

---

## The Problem

Four platforms identify saves by ROM filename stem: the save file is named `{romStem}.{ext}` in a known directory. To match a save to a game, you need to:
1. Know the ROM filename
2. Strip the extension to get the stem
3. Look for `{stem}.{save_ext}` in the save directory

This requires the ROM filename at registration time — which libsavesync doesn't currently have in a usable form for identity resolution.

## Option A: Library Reads ROM Filenames (New API Surface)

**How it works:**
- Add a new `SV_IDENTITY_STEM_MATCH` method
- Manifest declares: `stem_save_ext=mcd`, `stem_save_dir=memcards`
- At registration, the library scans `live_path/{stem_save_dir}/` for files matching `{romStem}.{stem_save_ext}`
- The ROM stem is derived from `opts->rom_path` (already available)
- If a match is found, the file's existence confirms the save belongs to this game

**API changes:**
- New enum value `SV_IDENTITY_STEM_MATCH`
- New manifest fields: `stem_save_ext`, `stem_save_dir`
- No new public function signatures — `rom_path` is already in `sv_register_opts_t`

**Pros:**
- Fully automatic — caller provides ROM path, library does the matching
- No caller-side logic needed
- Works with existing `sv_register_with_manifest()` API

**Cons:**
- Library now scans directories at registration time (I/O in identity path)
- Requires `rom_path` to be set — can't work without it
- The "match" is just "file exists with this name" — not a strong identity claim
- Doesn't handle fuzzy matching (word tokens, partial matches) that Freegosy uses

## Option B: Caller Always Supplies game_id (No API Change)

**How it works:**
- No new identity method
- Caller reads ROM filename, strips extension, uses result as `game_id`
- Manifest uses `identity=none` with a comment explaining the stem-matching convention

**API changes:**
- None — zero new code in libsavesync

**Pros:**
- Zero risk, zero new code
- Caller has full control over matching logic (can do fuzzy matching, word scoring, etc.)
- Matches Freegosy's actual approach (it does fuzzy word-token matching, not simple stem comparison)

**Cons:**
- Caller must implement stem extraction themselves
- No library-level automation for this common pattern

## Decision: Option B (Caller Supplies game_id) — FINAL

**Reasoning:**
1. Freegosy's actual stem matching is NOT simple — it does fuzzy word-token scoring, not just stem comparison. A library-level `SV_IDENTITY_STEM_MATCH` would only handle the simple case and still require the caller to implement the fuzzy part for real-world use.

2. The four affected platforms (DuckStation, mGBA, melonDS, RetroArch) all have their saves in well-known directories. The caller can trivially derive the stem from `rom_path` with a one-liner: `basename(rom_path, ext)`.

3. Option A adds I/O to the identity path and creates a partial solution that doesn't cover the real complexity. Option B is honest about what the library can and can't do.

4. The manifest documents the convention clearly:
   ```
   # Identity: stem-based (caller derives game_id from ROM filename)
   # Save files are named {romStem}.{ext} in this directory.
   # Caller should set game_id to the ROM filename without extension.
   identity=none
   ```

**If Option A is needed later,** the implementation path is clear: add `SV_IDENTITY_STEM_MATCH`, scan the save directory for `{romStem}.{ext}`, and return the stem as `game_id`. This is a small, self-contained addition that doesn't affect other identity methods.
