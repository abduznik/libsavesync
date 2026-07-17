# Contributing to libsavesync

## Build

```sh
make            # build static library + run all tests
make static     # build build/libsavesync.a only
make shared     # build build/libsavesync.so (adds -fPIC)
make clean      # remove build/
```

Compiler: `gcc` by default (override with `CC=clang`). Standard: `-std=gnu99`.

## Tests

```sh
make test       # builds and runs all four test suites
```

Current test suites (169 tests total):

| Suite | File | Covers | Count |
|-------|------|--------|-------|
| Basic | `test/test_basic.c` | DEFAULT mode: register, save, dedup, pull, retention, orphaning | 53 |
| Strategy | `test/test_strategy.c` | STRATEGY mode: manifest lifecycle, identity resolution (Tier 1/2/3), fallback chain, integration | 48 |
| Regressions | `test/test_regressions.c` | Historical bug regressions (serialize pointer, opaque accessor, dangling pointer, tiebreak) | 34 |
| Phase 3 Regressions | `test/test_phase3_regressions.c` | Phase 3 risk surfaces (coexistence, dedup after identity failure, shape mismatch, retention + game_id, metadata round-trip, hash-DB callback) | 34 |

### Where to add new tests

- **Functional tests for DEFAULT mode** → `test_basic.c`
- **Functional tests for STRATEGY mode** → `test_strategy.c`
- **Regression tests for fixed bugs** → `test_regressions.c` (or `test_phase3_regressions.c` for Phase 3-specific bugs)
- **New test files** — allowed, but add them to the `test` target in the Makefile

### Regression test convention (hard rule)

> **Every bug fix must ship a named regression test in the same commit as the fix.**

The test goes in `test_regressions.c` (or the relevant `*_regressions.c` file), named:

```
test_regression_<short_description>
```

The test must:
1. Fail against the pre-fix code
2. Pass against the fix
3. Be self-contained (creates its own temp directory, cleans up after itself)

PRs without a regression test for bug fixes will not be merged.

### Invariant vs Snapshot tests

See `docs/test-audit.md` for the full classification of every existing test.

**Guideline for new tests:** prefer **invariant-style** assertions over snapshot-style.

- **INVARIANT** — asserts a rule that must always hold regardless of implementation details. Example: "dedup must skip when hash matches most recent entry."
- **SNAPSHOT** — checks today's specific output/return value. Example: "sv_save returns SV_OK."

Invariant tests are more valuable because they survive refactors. Snapshot tests pin current behavior and break when behavior changes correctly.

## ABI Stability Rules

libsavesync ships a stable C ABI. These rules apply from the first version:

1. **No struct field reordering or removal.** Once a struct is shipped, its layout is frozen. New fields can only be appended at the end.

2. **Zero-default for all optional fields.** Every struct field that isn't structurally required (like `live_path`) must be optional. Callers leave unset fields zeroed/NULL. This is both a usability rule and an ABI-stability rule — new fields don't break old callers.

3. **Opaque handles at the API boundary.** No struct is passed by value across the API boundary for callers to allocate. Only opaque pointers + accessor functions, or read-only output snapshots.

4. **Append-only API additions.** New functions and enum values can be added. Existing function signatures must not change. Deprecated functions are marked but never removed.

5. **No implicit deletions.** No operation silently deletes data as a side effect of an unrelated operation. `sv_unregister()` orphans entries; `sv_delete_entry()` is the only hard-delete path.

## Code Style

- C99 (`-std=gnu99` for POSIX extensions)
- `-Wall -Wextra -pedantic` — code must compile cleanly under these flags
- Internal static functions are prefixed with the module name (e.g., `hash_file`, `find_entry`)
- Public API functions use the `sv_` prefix
- Test assertions use `TEST_ASSERT(condition, message)` macro

## Project Structure

```
include/savesync.h          Public API header (all types, enums, functions)
src/savesync.c              Complete implementation
test/test_basic.c           DEFAULT mode tests
test/test_strategy.c        STRATEGY mode tests
test/test_regressions.c     Historical bug regression tests
test/test_phase3_regressions.c  Phase 3 regression tests
docs/test-audit.md          Test strength classification (engineering notes)
docs/phase3-regression-risks.md  Phase 3 risk analysis (engineering notes)
docs/phase3-strategy.md     Phase 3 design spec
```
