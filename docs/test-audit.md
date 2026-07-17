# Step 1 — Test Strength Audit

## test_basic.c (53 tests)

| # | Test Description | Type | Notes |
|---|-----------------|------|-------|
| 1 | sv_init returns OK | SNAPSHOT | Checks one specific return value |
| 2 | sv_register returns non-NULL | INVARIANT | Handle must never be NULL on success |
| 3 | sv_register returns OK | SNAPSHOT | |
| 4 | sv_list_registrations returns OK | SNAPSHOT | |
| 5 | registration count is 1 | SNAPSHOT | Exact count check |
| 6 | listed ID matches handle ID | INVARIANT | Listed ID must always match the handle's ID |
| 7 | sv_read_registration returns OK | SNAPSHOT | |
| 8 | live_path matches | INVARIANT | Read-back must return what was registered |
| 9 | shape is FILE | INVARIANT | Shape must be preserved through registration |
| 10 | entry_count is 0 | INVARIANT | No entries before first save |
| 11 | sv_save returns OK | SNAPSHOT | |
| 12 | entry was created | INVARIANT | First save must always create an entry |
| 13 | no dedup on first save | INVARIANT | First save has nothing to dedup against |
| 14 | no evictions | INVARIANT | Retention_count=5, 0 entries → no eviction |
| 15 | sv_read_registration after save OK | SNAPSHOT | |
| 16 | entry_count is 1 after save | INVARIANT | Count must increment after save |
| 17 | sv_list_entries returns OK | SNAPSHOT | |
| 18 | entry count is 1 | SNAPSHOT | |
| 19 | listed entry ID matches save result | INVARIANT | Listed entry must match the one returned by save |
| 20 | sv_read_entry returns OK | SNAPSHOT | |
| 21 | integrity is OK | INVARIANT | Freshly saved entry must have integrity_ok=true |
| 22 | size matches | INVARIANT | Entry size must match actual file size |
| 23 | parent_id matches reg ID | INVARIANT | Entry's parent must be its registration |
| 24 | shape is FILE | INVARIANT | Shape must be preserved in entry metadata |
| 25 | magazine_slot_path is set | INVARIANT | After save, magazine path must be non-empty |
| 26 | sv_save (dedup) returns OK | SNAPSHOT | |
| 27 | dedup skipped (no change) | INVARIANT | Same content → dedup must fire |
| 28 | no entry created (dedup) | INVARIANT | Dedup must not create a new entry |
| 29 | still 1 entry after dedup save | INVARIANT | Entry count unchanged after dedup |
| 30 | sv_save (modified) returns OK | SNAPSHOT | |
| 31 | entry created after modification | INVARIANT | Different content → new entry must be created |
| 32 | no dedup (content changed) | INVARIANT | Changed content must not trigger dedup |
| 33 | 2 entries after second save | INVARIANT | Count must be 2 after two distinct saves |
| 34 | sv_save (force) returns OK | SNAPSHOT | |
| 35 | force creates new entry despite identical hash | INVARIANT | Force must bypass dedup |
| 36 | no dedup (forced) | INVARIANT | Force must not trigger dedup |
| 37 | 3 entries after force save | INVARIANT | Force save must increment count |
| 38 | sv_pull_select (override) returns OK | SNAPSHOT | |
| 39 | pull actually happened | INVARIANT | did_pull must be true on success |
| 40 | pulled content is V2 | INVARIANT | Pull must restore exact content of selected entry |
| 41 | sv_update_register (set retention=2) OK | SNAPSHOT | |
| 42 | sv_save (trigger retention) OK | SNAPSHOT | |
| 43 | at least 1 entry evicted | INVARIANT | Retention=2, 3 entries → must evict ≥1 |
| 44 | 2 entries remain after retention eviction | INVARIANT | After eviction, count must equal retention_count |
| 45 | sv_save (save V6) returns OK | SNAPSHOT | |
| 46 | sv_pull (report) returns CONFLICT when live changed | INVARIANT | Conflict must be reported when live differs from latest |
| 47 | conflict flag set | INVARIANT | conflicted flag must be true on CONFLICT return |
| 48 | sv_pull (override) returns OK | SNAPSHOT | |
| 49 | pull happened | INVARIANT | did_pull must be true |
| 50 | pulled content is V6 | INVARIANT | Override pull must restore the latest entry's content |
| 51 | sv_unregister completed | SNAPSHOT | |
| 52 | no entries listed under now-gone reg | INVARIANT | After unregister, entries must not belong to old reg |
| 53 | entries appear as orphans after unregister | INVARIANT | Orphaned entries must appear in orphan list |

## test_strategy.c (48 tests)

| # | Test Description | Type | Notes |
|---|-----------------|------|-------|
| 1 | sv_init returns OK | SNAPSHOT | |
| 2 | sv_manifest_create returns non-NULL | INVARIANT | Create must never return NULL |
| 3 | platform is 'ps2' after set | INVARIANT | Set/get must round-trip |
| 4 | emulator is 'pcsx2' after set | INVARIANT | Set/get must round-trip |
| 5 | shape is FILE after set | INVARIANT | Set/get must round-trip |
| 6 | identity_method is SERIAL_CNF after set | INVARIANT | Set/get must round-trip |
| 7 | save_path_template matches | INVARIANT | Set/get must round-trip |
| 8 | sv_manifest_free completed without crash | SNAPSHOT | |
| 9 | sv_manifest_save returns OK | SNAPSHOT | |
| 10 | sv_manifest_load returns OK | SNAPSHOT | |
| 11 | loaded manifest platform is 'psp' | INVARIANT | File I/O must preserve data |
| 12 | loaded manifest emulator is 'ppsspp' | INVARIANT | File I/O must preserve data |
| 13 | loaded manifest shape is DIRECTORY | INVARIANT | File I/O must preserve data |
| 14 | loaded manifest identity_method is SERIAL_SFO | INVARIANT | File I/O must preserve data |
| 15 | sv_manifest_load returns NOT_FOUND for missing file | INVARIANT | Missing file must return NOT_FOUND |
| 16 | sv_manifest_load handles malformed file gracefully | INVARIANT | Malformed input must not crash |
| 17 | sv_register_with_manifest returns non-NULL | INVARIANT | Valid registration must succeed |
| 18 | sv_register_with_manifest returns OK | SNAPSHOT | |
| 19 | sv_read_registration OK after strategy register | SNAPSHOT | |
| 20 | game_id was auto-detected (not empty) | INVARIANT | STRATEGY mode must resolve game_id |
| 21 | game_id matches SYSTEM.CNF serial 'TEST-00001' | INVARIANT | Resolved ID must match actual serial |
| 22 | register succeeds even with unparseable CNF | INVARIANT | Resolution failure must not prevent registration |
| 23 | game_id is empty when SYSTEM.CNF is too short | INVARIANT | Short file must not produce a game_id |
| 24 | register succeeds when pattern doesn't match | INVARIANT | Pattern mismatch must not prevent registration |
| 25 | game_id is empty when pattern doesn't match | INVARIANT | Mismatch must not produce a game_id |
| 26 | register with CHECKSUM identity returns non-NULL | INVARIANT | Checksum registration must succeed |
| 27 | sv_read_registration OK for checksum-based reg | SNAPSHOT | |
| 28 | register with PLUGGABLE identity returns non-NULL | INVARIANT | Pluggable registration must succeed |
| 29 | register succeeds when Tier 1 fails (no CNF file) | INVARIANT | Missing file must not prevent registration |
| 30 | game_id is empty when Tier 1 fails and no Tier 2/3 configured | INVARIANT | Full failure must leave game_id empty |
| 31 | strategy register returns non-NULL | INVARIANT | |
| 32 | sv_save under STRATEGY mode returns OK | SNAPSHOT | |
| 33 | entry created under STRATEGY mode | INVARIANT | Save must work under STRATEGY |
| 34 | sv_list_entries under STRATEGY mode OK | SNAPSHOT | |
| 35 | 1 entry after save under STRATEGY mode | INVARIANT | Count must be correct |
| 36 | sv_save (modified) under STRATEGY mode OK | SNAPSHOT | |
| 37 | new entry created for modified save | INVARIANT | Modified content must create new entry |
| 38 | 2 entries after second save under STRATEGY mode | INVARIANT | Count must be correct |
| 39 | sv_pull under STRATEGY mode returns OK | SNAPSHOT | |
| 40 | pull happened under STRATEGY mode | INVARIANT | did_pull must be true |
| 41 | sv_pull_select under STRATEGY mode returns OK | SNAPSHOT | |
| 42 | pulled V1 via pull_select under STRATEGY mode | INVARIANT | Pull must restore correct content |
| 43 | register with STRATEGY mode but NULL manifest succeeds | INVARIANT | NULL manifest must not crash |
| 44 | register returns OK (falls back to DEFAULT behavior) | SNAPSHOT | |
| 45 | sv_read_registration OK for fallback reg | SNAPSHOT | |
| 46 | game_id is empty (no manifest to resolve) | INVARIANT | No manifest → no game_id |
| 47 | mode is STRATEGY even without manifest | INVARIANT | Mode must be preserved even if manifest is NULL |
| 48 | sv_save works in STRATEGY mode without manifest | SNAPSHOT | |

## Summary

| File | INVARIANT | SNAPSHOT | Total |
|------|-----------|----------|-------|
| test_basic.c | 32 | 21 | 53 |
| test_strategy.c | 28 | 20 | 48 |
| **Total** | **60** | **41** | **101** |
