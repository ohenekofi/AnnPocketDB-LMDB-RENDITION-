# Sanitizer builds

This closes the self-disclosed gap: *"no TSan/ASan-instrumented CI, so the
MVCC/free-list concurrency claims aren't independently verified."*

## Why both a fast tier and a soak tier

A sanitizer only catches a race if the buggy interleaving actually happens
during the run. The existing fast test suite (`test_mvcc`, `test_2pc`,
`test_mplay_fuzzer`, etc.) runs in seconds and proves specific, fixed
scenarios are correct — that's necessary but not sufficient. TSan's value is
proportional to how much real scheduling entropy it sees, so:

- **Fast tier** (`sanitize` job, every PR): builds under `thread` and
  `address+undefined`, runs the existing fast suite. Catches anything the
  existing tests would already exercise, just now with instrumentation. This
  is a correctness *regression* gate, not a discovery tool by itself.
- **Soak tier** (`soak-nightly` job, cron + manual dispatch): same builds,
  but runs `test_soak_concurrent` (in-process) and `test_mplay_soak`
  (cross-process) for up to an hour, with `LABELS soak`. This is the actual
  discovery tool — sustained, randomized, overlapping load against the
  free-list, meta-page flip, and reader table.

Treat a green fast tier as "no regressions in known scenarios." Only treat a
green soak tier as evidence toward "the MVCC/free-list claims hold up."

`crash_worker`/`test_crash_consistency` are also built with
`annpocketdb_apply_sanitizers` applied (same as every other test binary), so
the WAL/crash-recovery code path gets the same instrumentation as everything
else in the fast tier — this catches ordinary data races in that code, but
is a separate concern from the "did recovery actually reconstruct the right
state after a real kill" question the crash-consistency test itself answers
(see "What this does *not* cover" below).

## Running locally

```bash
# ThreadSanitizer — the primary tool for the disclosed gap. Clang required
# (GCC's TSan works too but Clang's diagnostics are better for this codebase's
# std::atomic/mmap-heavy style).
CC=clang CXX=clang++ cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug -DANNPOCKETDB_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure -LE soak       # fast pass
ANNPOCKETDB_SOAK_SECONDS=600 ctest --test-dir build-tsan --output-on-failure -L soak   # 10-min local soak

# AddressSanitizer + UBSan — memory safety + undefined behavior
CC=clang CXX=clang++ cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug -DANNPOCKETDB_SANITIZE=address+undefined
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure -LE soak
```

On Windows, only ASan is available (MSVC's TSan is preview-only):

```powershell
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DANNPOCKETDB_SANITIZE=address
cmake --build build-asan --config Debug -j
ctest --test-dir build-asan -C Debug --output-on-failure -LE soak
```

## Interpreting a TSan report

TSan reports look like:

```
WARNING: ThreadSanitizer: data race (pid=...)
  Write of size 8 at 0x... by thread T2 (mutexes: write M1):
    #0 annpocketdb::core::FreeList::retire(...) free_list.cpp:142
  Previous read of size 8 at 0x... by thread T5:
    #0 annpocketdb::core::PageManager::getPage(...) page_manager.cpp:88
  Location is heap block of size ... allocated by thread T1:
```

The two stacks are the two racing accesses. `mutexes: write M1` on one side
but not the other is the classic signature of "this access is actually
protected in the writer path but the reader path reads it without the lock
(or without an acquire fence) some MVCC readers are supposed to be lock-free
against" — the exact bug class this whole effort exists to catch. Do not
paper over this with a suppression (see `ci/tsan.supp`); it means either the
reader path needs an atomic/acquire-release pair, or the free-list retire
epoch isn't actually excluding that reader yet.

`ci/tsan.supp`, `ci/lsan.supp`, and `ci/ubsan.supp` currently ship empty
(placeholder files with an explanatory comment only) — no sanitizer run has
happened yet to know what, if anything, legitimately needs suppressing.
Populate them only with entries for confirmed false positives (e.g. a
third-party vendored library doing something the sanitizer can't see through
but is actually safe), never as a way to silence a real finding.

## What this does *not* cover

- **Cross-process shared-memory races** in the reader-table lock file. TSan
  instruments one process's address space; it cannot see a race between two
  separate OS processes touching the same `mmap(MAP_SHARED, ...)` region.
  `test_mplay_soak` gives this path sustained real multi-process load and
  relies on the worker binary being *itself* built under TSan/ASan (it is,
  since `mplay_worker` goes through the same `annpocketdb_apply_sanitizers`
  call), but a genuine cross-process race can still only be caught if it
  also manifests as a same-process race in at least one of the workers, or
  by the value-corruption check `mplay_worker` does itself. A full
  cross-process race detector is out of scope here.
- **Crash-mid-write consistency.** That's already covered separately by
  `test_crash_consistency`/`crash_worker` (real process kills at random and
  precise timing, prefix-property verification on recovery) — not something
  a sanitizer catches, since sanitizers instrument a running process, not a
  killed one. Building those binaries under a sanitizer (see above) adds
  ordinary same-process race detection on top, but doesn't replace the
  crash-consistency test's own job.
- **Performance impact of TSan-required serialization.** TSan itself
  enforces extra synchronization to track happens-before edges, which can
  mask certain benign lock-free patterns as "probably fine" simply because
  TSan's own overhead reduces the interleaving space. This is a known
  limitation, not specific to this setup.
