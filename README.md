# libsavesync

A pure C, embeddable save-sync engine for emulators. It manages the precise lifecycle of emulator save files: identifying which save belongs to which game, safely versioning it locally, and optionally handing it off to external transports for cross-device sync.

It ships as two artifacts:
- **A linkable C ABI library** (`libsavesync.a`) — embed directly into your app, plugin, or tool
- **A standalone IPC binary** (`savesync-ipc`) — talk to it from any language via newline-delimited JSON over stdin/stdout

libsavesync is not an emulator, launcher, cloud service, or orchestration server. It is a local, passive, embeddable save lifecycle manager — infrastructure other apps link against or shell out to.

## Supported Platforms

| Emulator | Platform | Identity Method | Validation Status |
|----------|----------|-----------------|-------------------|
| Dolphin (GameCube) | GC | `rom_header` — reads game ID from ROM header | Validated against real save data |
| Ryujinx | Switch | `text_pattern` — scans cnmt for title ID | Validated against real save data |
| PPSSPP | PSP | `text_pattern` — extracts from save directory name | Validated against real save data |
| RPCS3 | PS3 | `text_pattern` — extracts from save directory name | Validated against real save data |
| PCSX2 (Folder mode) | PS2 | `text_pattern` — extracts serial from `SYSTEM.CNF` | Validated against real save data |
| PCSX2 (File mode) | PS2 | Caller-supplied game ID — shared memcard treated as a single opaque backup unit | Manifest exists, per-game extraction out of scope |
| DuckStation | PS1 | Caller-supplied game ID — stem-based convention documented in manifest | Manifest exists, not validated against real local data |
| RetroArch (SNES core) | SNES | Caller-supplied game ID — stem-based convention documented in manifest | Manifest exists, not validated against real local data |

## OS Support

| OS | Status | Notes |
|----|--------|-------|
| Linux | Supported | Primary development platform |
| macOS | Supported | Tested via CI |
| Windows | Supported | Tested via CI (MinGW-w64). IPC test skipped on Windows (`fork`/`waitpid` unavailable). |

## Quick Start

### Path A: Link the C ABI

```c
#include "savesync.h"

// Initialize with a base directory for versioned storage
sv_init("/path/to/savesync-data");

// Load an emulator manifest (auto-detects game identity)
sv_manifest_t *manifest = sv_manifest_create();
sv_manifest_load("manifests/dolphin_gc.cfg", manifest);

// Register a save — game ID is extracted automatically from the ROM header
sv_register_opts_t opts = {0};
opts.live_path = "/path/to/save/TEST01";
sv_status_t st;
sv_registration_t *reg = sv_register_with_manifest(&opts, manifest, &st);

// Snapshot the current save into the versioned magazine
sv_save_result_t save_result;
sv_save(reg, NULL, &save_result);

// Pull (restore) the latest saved version
sv_pull_result_t pull_result;
sv_pull(reg, NULL, &pull_result);

sv_shutdown();
```

Build and link:

```sh
make static                    # produces build/libsavesync.a
# link with: -Lbuild -lsavesync -Iinclude
```

### Path B: Use the IPC binary (any language)

```python
import json, subprocess, tempfile, os

proc = subprocess.Popen(["build/savesync-ipc"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def send(method, params=None):
    msg = {"id": method, "method": method}
    if params: msg["params"] = params
    proc.stdin.write(json.dumps(msg) + "\n")
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())

with tempfile.TemporaryDirectory() as tmpdir:
    os.makedirs(os.path.join(tmpdir, "live_save"))
    with open(os.path.join(tmpdir, "live_save", "data.bin"), "wb") as f:
        f.write(b"SYNTHETIC_SAVE_V1")

    send("init", {"base_path": os.path.join(tmpdir, "data")})
    resp = send("manifest_load", {"path": "manifests/dolphin_gc.cfg"})
    mid = resp["result"]["manifest_id"]

    resp = send("register_with_manifest", {
        "manifest_id": mid,
        "live_path": os.path.join(tmpdir, "live_save", "data.bin"),
        "shape": "file",
    })
    rid = resp["result"]["registration_id"]

    send("save", {"registration_id": rid})
    send("shutdown")
```

See [examples/python_client.py](examples/python_client.py) for the full example.

## Build

```sh
make              # build static lib + IPC binary + run all tests
make static       # build build/libsavesync.a only
make ipc          # build build/savesync-ipc only
make test         # build and run all test suites (360 tests)
make clean        # remove build/
```

Compiler: `gcc` by default (override with `CC=clang`). Standard: `-std=gnu99`.

## Tests

360 tests across 7 test files:

| Suite | File | Count |
|-------|------|-------|
| Basic | `test/test_basic.c` | 53 |
| Strategy | `test/test_strategy.c` | 48 |
| Regressions | `test/test_regressions.c` | 34 |
| Phase 3 Regressions | `test/test_phase3_regressions.c` | 72 |
| Real Manifests | `test/test_real_manifests.c` | 96 |
| ROM Header Identity | `test/test_rom_header_identity.c` | 22 |
| IPC Integration | `test/test_ipc.c` | 35 |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for project conventions, test-first workflow, and ABI stability rules.

## Links

- [IPC Protocol v1](docs/ipc-protocol-v1.md) — full NDJSON protocol specification
- [CI/CD & Release Pipeline](docs/ci-cd-release.md) — multi-platform build and release automation
- [Project Specification](save-sync-engine-spec.md) — design philosophy and layer architecture
- [Wiki](https://github.com/abduznik/libsavesync/wiki) — API reference, roadmap, examples

## License

See [LICENSE](LICENSE).
