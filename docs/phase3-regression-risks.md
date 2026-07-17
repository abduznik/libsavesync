# Phase 3 Regression Risk Analysis

Every shared code path between STRATEGY-mode and DEFAULT-mode code is a regression surface.

## 1. Registration Entry Point

**Shared code:** `sv_register_with_manifest()` calls `sv_register()` internally — no, it duplicates the registration logic. But both paths call:
- `generate_unique_id()`
- `add_entry()`, `link_entry_to_reg()`
- `metadata_flush()`

**Risk:** If `sv_register_with_manifest()` has a bug in its duplicated registration path, it could corrupt the metadata store in a way that also affects DEFAULT-mode registrations (since they share the same `g_ctx`).

**Test needed:** Register in DEFAULT, register in STRATEGY, verify both can coexist and their entries don't collide.

## 2. Identity Resolution Failure → DEFAULT Hashing

**Shared code:** When identity resolution fails (Tier 1/2/3 all miss), `sv_register_with_manifest()` falls back to leaving `game_id` as NULL. The `sv_save()` function then uses `hash_file()` or `hash_data()` for dedup regardless of mode.

**Risk:** Identity resolution reads the same file that `hash_file()` reads. If the manifest's `serial_offset`/`serial_length` causes a partial read that corrupts file state (e.g., seek pointer), dedup hashing could break. Currently `fopen`/`fread`/`fclose` per call, so this is unlikely — but worth testing.

**Test needed:** Register with manifest, save, verify dedup works identically to DEFAULT mode.

## 3. Manifest-Driven Shape Override

**Shared code:** `sv_register_with_manifest()` can override `reg->shape` from the manifest if the caller passes `SV_SHAPE_UNKNOWN`.

**Risk:** If manifest shape differs from actual file type, `copy_to_magazine()` uses the wrong copy strategy (e.g., `atomic_copy_dir` on a file).

**Test needed:** Register with manifest shape=FILE for a directory → should still work (atomic_copy_dir treats it as copy_to_magazine with the declared shape). Verify no crash.

## 4. sv_update_register Relocation with Manifest

**Shared code:** `sv_update_register()` works on `reg->live_path` regardless of mode. If a registration was created via `sv_register_with_manifest()`, the relocation logic should work identically.

**Risk:** None identified — relocation doesn't use manifest data. But worth a smoke test.

## 5. Retention Eviction with Identity Metadata

**Shared code:** `run_retention()` evicts entries from `reg->entries[]`. STRATEGY-mode entries carry the same structure as DEFAULT entries — identity metadata is on the registration, not the entry.

**Risk:** If retention evicts the only entry that "anchors" a registration's identity (e.g., the entry whose content hash was used for a hash-DB lookup), future identity resolution could fail. But identity is resolved at registration time, not at save time — so this is not a real risk.

**Test needed:** Register with manifest, save multiple times (trigger retention), verify game_id persists after eviction.

## 6. Metadata Store Serialization

**Shared code:** `serialize_reg()` and `deserialize_reg()` handle both DEFAULT and STRATEGY registrations through the same code path. STRATEGY registrations have `game_id` set.

**Risk:** If `game_id` contains characters that break the TLV serialization (e.g., null bytes, very long strings), metadata could be corrupted. The serialization uses `strndup` with explicit length, so this should be safe — but worth testing with a long game_id.

**Test needed:** Register with manifest, verify metadata round-trip preserves game_id.

## 7. Pluggable Hash-DB Callback During Registration

**Shared code:** `resolve_identity()` calls `manifest->hash_db_lookup()` which is caller-supplied.

**Risk:** A buggy callback could crash, return garbage, or corrupt memory. The library should be resilient — it already checks return value and only uses the result if `SV_OK`.

**Test needed:** Register with a callback that returns `SV_ERR_NOT_FOUND` → game_id stays empty, no crash.

## Summary of Required Tests

| Risk | Test Description |
|------|-----------------|
| 1 | DEFAULT + STRATEGY registrations coexist without metadata corruption |
| 2 | Identity resolution failure doesn't break dedup hashing |
| 3 | Manifest shape override doesn't crash on mismatched file type |
| 4 | sv_update_register works on STRATEGY-mode registrations |
| 5 | Retention eviction preserves game_id on the registration |
| 6 | Metadata round-trip preserves game_id with special characters |
| 7 | Hash-DB callback returning NOT_FOUND doesn't crash |
