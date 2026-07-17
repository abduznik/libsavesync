# IPC Protocol v1

## Overview

A minimal JSON-over-stdio protocol for the `savesync` IPC binary. This is the Layer 1 entry point: a standalone binary that wraps the C ABI and speaks newline-delimited JSON, enabling any language (Python, JS, shell via jq, etc.) to use savesync without linking against the C library.

## Transport

- **Encoding**: Newline-delimited JSON (NDJSON) over stdin/stdout
- **Encoding rationale**: NDJSON is the simplest format that every target language can produce and consume without extra dependencies. Python's `json` module, JavaScript's `JSON.parse`, and shell's `jq` all handle it natively. No framing, length-prefix, or binary encoding needed.
- **One JSON object per line in, one JSON object per line out.** Each request produces exactly one response.
- **Errors never crash the process.** Malformed input, unknown methods, and internal errors all produce a JSON error response; the process continues reading.

## Message Shape

### Request
```json
{"id": "req-1", "method": "init", "params": {"base_path": "/tmp/saves"}}
```

- `id` (string): Client-chosen request identifier. Echoed in the response. Lets a client pipeline multiple in-flight requests if it wants to, even though v1 processes them strictly sequentially.
- `method` (string): Method name (see Method Surface).
- `params` (object): Method-specific parameters. Omitted if the method takes no parameters.

### Response
```json
{"id": "req-1", "result": {"ok": true}}
```
or
```json
{"id": "req-1", "error": {"code": -3, "message": "invalid argument"}}
```

- `id` (string): Matches the request `id`.
- `result` (object): Present on success. Shape depends on method.
- `error` (object): Present on failure. `code` is a negative integer (see Error Mapping). `message` is a human-readable string.

## Error Mapping

| sv_status_t | Code | Message |
|---|---|---|
| SV_OK (0) | 0 | ok |
| SV_ERR_GENERIC (-1) | -1 | generic error |
| SV_ERR_INVALID_ARG (-2) | -2 | invalid argument |
| SV_ERR_NOT_FOUND (-3) | -3 | not found |
| SV_ERR_IO (-4) | -4 | I/O error |
| SV_ERR_OUT_OF_MEMORY (-5) | -5 | out of memory |
| SV_ERR_CONFLICT (-6) | -6 | conflict |
| SV_ERR_UNAVAILABLE (-7) | -7 | unavailable |
| (parse error) | -100 | parse error |
| (unknown method) | -101 | unknown method |

## Method Surface (v1)

### `init`
Initialize the library.
- **params**: `{"base_path": "<path>"}`
- **result**: `{"ok": true}`
- **c ABI**: `sv_init(base_path)`

### `manifest_load`
Load a manifest from a file path.
- **params**: `{"path": "<path>"}`
- **result**: `{"manifest_id": "<id>"}`
- **c ABI**: `sv_manifest_load(path, manifest)` — returns an opaque manifest handle stored internally, its integer index returned as `manifest_id`.

### `register_with_manifest`
Register a save using a manifest. Supply `live_path` (required), optional `rom_path`, `game_id`. The manifest's identity resolution extracts game_id if not supplied.
- **params**: `{"manifest_id": "<id>", "live_path": "<path>", "rom_path": "<path>", "game_id": "<id>", "shape": "directory", "retention_count": 5}`
- **result**: `{"registration_id": "<id>"}`
- **c ABI**: `sv_register_with_manifest(opts, manifest, &status)`

### `save`
Save (snapshot) the current state of a registration's live files into the magazine.
- **params**: `{"registration_id": "<id>"}`
- **result**: `{"entry_id": "<id>", "entry_created": true, "dedup_skipped": false}`
- **c ABI**: `sv_save(reg, NULL, &result)`

### `pull`
Pull (restore) the latest entry from the magazine to the live path.
- **params**: `{"registration_id": "<id>"}`
- **result**: `{"conflicted": false, "did_pull": true, "did_backup": false}`
- **c ABI**: `sv_pull(reg, NULL, &result)`

### `pull_select`
Pull a specific entry by ID.
- **params**: `{"registration_id": "<id>", "entry_id": "<id>"}`
- **result**: `{"conflicted": false, "did_pull": true, "did_backup": false}`
- **c ABI**: `sv_pull_select(reg, entry_id, NULL, &result)`

### `list_entries`
List all entries (save snapshots) for a registration.
- **params**: `{"registration_id": "<id>"}`
- **result**: `{"entries": [{"id": "<id>", "label": "...", "mtime": 1234567890, "size_bytes": 4096}, ...]}`
- **c ABI**: `sv_list_entries(reg_id, ids, max, &count)` + `sv_read_entry(id, &info)` for each

### `list_registrations`
List all active registrations.
- **params**: `{}` (empty)
- **result**: `{"registrations": [{"id": "<id>", "game_id": "...", "platform": "...", "emulator": "..."}, ...]}`
- **c ABI**: `sv_list_registrations(ids, max, &count)` + `sv_read_registration(id, &info)` for each

### `unregister`
Remove a registration (does not delete live files or magazine entries).
- **params**: `{"registration_id": "<id>"}`
- **result**: `{"ok": true}`
- **c ABI**: `sv_unregister(reg)`

### `shutdown`
Shut down the library and exit cleanly.
- **params**: `{}` (empty)
- **result**: `{"ok": true}` — process exits after sending this response.
- **c ABI**: `sv_shutdown()`

## Explicitly Out of Scope for v1

- Layer 6 transport: `push_external`, `pull_external`
- `update_register`, `reparent_entry`, `delete_entry`
- Manifest setters (platform, emulator, shape, identity, etc.)
- `sv_read_entry` as a standalone method (exposed indirectly via `list_entries`)

These can be added in v2 without breaking v1 clients — new methods are additive.

## Example Session

```
→ {"id":"1","method":"init","params":{"base_path":"/tmp/saves"}}
← {"id":"1","result":{"ok":true}}

→ {"id":"2","method":"manifest_load","params":{"path":"manifests/ppsspp.cfg"}}
← {"id":"2","result":{"manifest_id":"m0"}}

→ {"id":"3","method":"register_with_manifest","params":{"manifest_id":"m0","live_path":"/tmp/saves/ULUS10509001","shape":"directory","retention_count":5}}
← {"id":"3","result":{"registration_id":"r0"}}

→ {"id":"4","method":"save","params":{"registration_id":"r0"}}
← {"id":"4","result":{"entry_id":"e0","entry_created":true,"dedup_skipped":false}}

→ {"id":"5","method":"list_entries","params":{"registration_id":"r0"}}
← {"id":"5","result":{"entries":[{"id":"e0","label":"","mtime":1721234567,"size_bytes":4096}]}}

→ {"id":"6","method":"pull","params":{"registration_id":"r0"}}
← {"id":"6","result":{"conflicted":false,"did_pull":true,"did_backup":false}}

→ {"id":"7","method":"shutdown"}
← {"id":"7","result":{"ok":true}}
```
