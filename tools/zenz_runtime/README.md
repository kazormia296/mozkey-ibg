# Mozkey Zenz llama.cpp runtime lock

This is a **checked, explicit pin**, not a moving upstream dependency.  It
separately records Windows (`v0.7.0` at integration time) and macOS (`b10268`
at integration time), because the two platforms currently ship different
upstream revisions.  It does not update either platform by itself.

**Authoritative current release pin:**
`src/zenz_runtime/llama_runtime.lock.json`.

## Read-only integrity audit

```powershell
cd C:\Users\Makoto\dev\mozc
python tools/zenz_runtime/runtime_lock.py check
python -m unittest discover -s tools/zenz_runtime -p 'test_*.py' -v
```

The check verifies the tracked Windows x64/ARM64 executable hashes and PE
architectures, tracked model hash, tracked tokenizer and dual-liveness patches,
and the current pinned versions/hashes in Windows CI, Windows lifecycle audit,
Windows contract and notices, and three macOS build/staging/package scripts plus
macOS source README.  It is **read-only**, runs on any Python 3.10+ system, and
needs no external packages.

## Updating llama.cpp in a future change

1. Use a **new branch** and audit an exact upstream release tag/40-character
   commit, security fixes, required tokenizer/dual-liveness patches, license and
   new CMake/build compatibility.  Preserve historic test fixtures and installer
   predecessor identity.  Never use moving `HEAD` as a release pin.
2. Build Windows x64/ARM64 candidates separately, check native architecture,
   imports and CRT requirements, compare model outputs and latency against the
   tracked version; run macOS universal/liveness tests when updating macOS.
   Only after these succeed, stage the tested binaries through a guarded process.
3. Change the **current platform's** upstream and candidate hash entries in
   `src/zenz_runtime/llama_runtime.lock.json`.  Keep the unchanged platform's
   upstream pin intact.  Recompute patched-source hashes from the exact target
   commit and named patches; do not infer them from earlier commits.
4. Preview the pinned-text updates (does not write):

   ```powershell
   python tools/zenz_runtime/runtime_lock.py sync --diff
   ```

   If correct, apply **only the controlled current-pin replacements**:

   ```powershell
   python tools/zenz_runtime/runtime_lock.py sync --write --diff
   python tools/zenz_runtime/runtime_lock.py check
   ```

   A candidate-executable or patch hash mismatch aborts before any replacement.
   On sync, the listed pinned assignment values, source identity lines, and
   selected provenance fields are mechanically synchronized.  Historical
   `$ExpectedBaselineLlamaSha` and `UpgradeCode` are not touched.
5. **Manually review and update** release-specific documentation and quality/
   performance narratives, especially `WINDOWS_ZENZ_RUNTIME_CONTRACT.md`,
   `THIRD_PARTY_NOTICES.md`, macOS README and notices.  The sync tool updates
   only explicitly recognized current pins, **not prose or test claims**.  A
   different upstream release requires new measurements and license review;
   copying previous semantic/installed-package claims would be incorrect.
6. Run the check and tests again; review the diff, build the MSI/PKG and verify
   installed payload hashes, native process execution, firewall, upgrade and
   uninstall on dedicated runners.  Release ProductVersion/ProductCode and
   signature/notarization rules are independent controls and must be reviewed.

The `Zenz runtime lock integrity (read-only)` GitHub workflow runs the same
checks on each push/PR.  It does **not** mutate CI artifacts, MSI, PKG, Git, or
installed software.  An upstream version upgrade is never automatically
approved merely because the lock and staged hashes match.

## Scope boundaries

- Windows **installed-payload** verification still compares the physical
  installed `llama-server.exe` with the expected current hash; a lock match does
  not replace this end-to-end test.
- The historical MSI upgrade baseline is separate from the current runtime and
  must remain pinned to the historical build.
- The dual-liveness patch is active only on Apple platforms.  The Windows x64
  build historically includes the source patch but does not activate its
  conditional implementation; the ARM64 portable build does not apply it.
- macOS generated Universal executables/PKGs are not tracked in this lock as
  current files, and are separately audited against the stage/build contract.
- This initial migration uses **synchronized literal pins** in the existing
  consumers instead of changing executable build/install logic.  This is
  intentional to keep the first migration low-risk: the lock is the single
  *editing* source, and the CI check enforces that all controlled copies match.
