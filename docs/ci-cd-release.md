# CI/CD & Release Pipeline

## Overview

libsavesync uses GitHub Actions for continuous integration and automated releases. Every push and PR is tested across Linux, macOS, and Windows. Releases are triggered by version tags.

## Workflows

### CI (`ci.yml`)

**Trigger:** Every push to `main` and every PR targeting `main`.

**Matrix:**
| Platform | Runner | Compiler |
|----------|--------|----------|
| Linux | `ubuntu-latest` | `gcc` |
| macOS | `macos-latest` | `cc` (clang) |
| Windows | `windows-latest` | `gcc` (MinGW-w64) |

**Steps per platform:**
1. Build the static library (`make static`) and IPC binary (`make ipc`)
2. Run the full test suite (`make test`)
3. Verify the IPC binary starts cleanly

**Key behavior:** `fail-fast: false` — a failure on one platform does not cancel the others. All platform results are visible in a single CI run.

### Release (`release.yml`)

**Trigger:** Push of a version tag matching `v*` (e.g., `v0.1.0`, `v1.2.3`).

**Three-phase pipeline:**

#### Phase 1: Test gate
The full test suite runs on all three platforms. **No packaging or uploading happens if any platform's tests fail.** This is the release gate.

#### Phase 2: Build & package
Each platform is built and packaged on its native runner (not cross-compiled). Artifacts are uploaded per-platform:
   - `libsavesync-{version}-{platform}-{arch}.tar.gz`
   - Each archive contains:
     - `lib/libsavesync.a` — static library
     - `bin/savesync-ipc` — standalone IPC binary (`savesync-ipc.exe` on Windows)
     - `include/savesync.h` — public C header

#### Phase 3: Publish
All platform artifacts are downloaded and a GitHub Release is created with all archives attached and auto-generated release notes.

## Per-Platform Status

### Linux (ubuntu-latest)
Expected: ✅ Full support. Builds and tests pass.

### macOS (macos-latest)
Expected: ✅ Full support. Builds and tests pass. The codebase uses `_DARWIN_C_SOURCE` for compatibility.

### Windows (windows-latest)
Expected: ✅ Full support. Builds and tests pass (MinGW-w64).

POSIX APIs (`/dev/urandom`, `mkdir`, `rename`, `mkdtemp`) have been replaced with cross-platform equivalents using `#ifdef _WIN32` guards and Win32 API calls where needed (`CreateFile`, `MoveFileEx`, `windows.h`). The IPC test (`fork`/`waitpid`) is skipped on Windows.

## Local Development

```bash
# Build everything
make all

# Run the full test suite
make test

# Build just the IPC binary
make ipc

# Clean
make clean
```

## Creating a Release

```bash
# Tag and push
git tag v0.1.0
git push origin v0.1.0
```

This triggers the release workflow. Monitor the Actions tab for progress.

## Archive Contents

Each release archive follows this structure:

```
libsavesync-{version}-{platform}-{arch}/
├── lib/
│   └── libsavesync.a
├── bin/
│   └── savesync-ipc          (savesync-ipc.exe on Windows)
└── include/
    └── savesync.h
```
