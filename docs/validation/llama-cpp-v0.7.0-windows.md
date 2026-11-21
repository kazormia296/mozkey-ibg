# Windows llama.cpp v0.7.0 integration validation

## Scope and evidence identity

- Windows runtime: upstream llama.cpp `v0.7.0`, commit `620d2225cc436ab0b001e3200e590e3b286ef9ab`.
- Mozkey user-facing release at the start of the upgrade: `v0.7.7` (this is distinct from the llama.cpp runtime version).
- Runtime binaries, patch set and model identity are pinned in [`llama_runtime.lock.json`](../../src/zenz_runtime/llama_runtime.lock.json).
- This is a version-specific record. The ongoing requirements are defined in [Windows Zenz Runtime Contract](../../src/win32/installer/zenz_runtime/WINDOWS_ZENZ_RUNTIME_CONTRACT.md).

## Japanese conversion quality and latency (Windows x64)

- Quality comparison: tracked b10437 `FE9015591099ACDA45A37D8D8D83C1B1EABA9305CE3FFAA6B3808F9FA2953251` versus v0.7.0 `35B953C97F870CBD7426DC865783BE36090F841C273AE779BD098E9F080F3735`; 119 cases x 4 counterbalanced paired rounds, 118/119 stable same outputs, 116/119 reference-exact for both, mean latency 62.69 -> 53.74 ms (-14.28%). These figures are not a definitive accuracy estimate.

### Human review of changed output

- Only output-different case: `model_full`, input `ローカルエルエルエムデニホンゴヘンカン`, reference `ローカルLLMで日本語変換` / `ローカルllmで日本語変換`; b10437 produced `lawからllmで日本語変換`, v0.7.0 produced `lawかllmで日本語変換`. Both are incorrect. Thus no reference-exact regression was detected in this comparison; this does **not** establish perfect output parity or universal accuracy. The other two reference-mismatching cases had identical outputs across the two runtimes.

The other two reference-mismatching cases were `fork_domain` and `long_sentence_5`; their outputs were unchanged between the runtimes. The original evaluation was a finite 119-case sample, not a claim of universal accuracy or identical output for all inputs.

## CI and Windows ARM64 MSI evidence

- GitHub Actions Windows CI: https://github.com/koyasi777/mozkey/actions/runs/40625814079 (all jobs successful), including the native ARM64 installed-MSI and installer lifecycle audits. Runtime lock CI: https://github.com/koyasi777/mozkey/actions/runs/40625814041 (PASS).

- In the cited Windows CI run, the native ARM64 MSI installed-runtime checks, cross/native package parity audit, and native installer lifecycle audit all passed.
- ARM64 lifecycle checks included predecessor-to-current normal upgrade, old runtime DLL removal, ProductCode transition with stable UpgradeCode, scorer context E2E (2/2), firewall (7/7), same-package second installation, and final uninstall cleanup.
- Tested on CI runners; compatibility across every physical ARM64 CPU is not established.

## Windows x64 physical-machine MSI upgrade (2026-10-28)

- Windows x64 GitHub Actions MSI artifact (run 40625814079, `Mozc64_x64.msi`) SHA256: `35FF576928DCA5971292C945A44F4851192A2F4541D632D1F5844CD8E9DF281B`.
- Real-machine upgrade baseline: installed Mozkey v0.7.7 MSI technical ProductVersion `3.33.6153.100`, ProductCode `{547FDF01-0237-4177-8A49-DA45164F607A}`, UpgradeCode `{DD94B570-B5E2-4100-9D42-61930C611D8A}`; old installed x64 `llama-server.exe` SHA256 `A826D6BC47645A9E9907FEEA49109C0FF25D49CFD268D3349AC5F9BD6DBAAEC0`.
- Real-machine upgrade operation: normal MSI `/i` over the existing product, **without prior uninstall** and without forced `REINSTALL=ALL`; MSI exit code `3010` (success/reboot required), followed by a Windows restart.
- Post-reboot audit: current ProductVersion `100.7.7`, ProductCode `{766D0248-84CC-4F34-A7A0-D174E91DC841}`, same UpgradeCode, old ProductCode absent, installed `llama-server.exe` SHA256 `35B953C97F870CBD7426DC865783BE36090F841C273AE779BD098E9F080F3735` with PE x64 machine `0x8664`, firewall rules 7/7. Audit result: `MOZKEY_X64_MSI_UPGRADE_POSTCHECK=PASS`.
- Installed-IME manual smoke: user reported no apparent problems after the upgrade; both `mozc_zenz_scorer.exe` and `llama-server.exe` were observed running from the installed Mozkey directory, with the model supplied from the installed `models` directory and loopback HTTP endpoint `127.0.0.1`. This is a manually reported functional check, not a complete automated x64 installer lifecycle or security audit. No API key is recorded here.

## Scope limits and future regression references

- The x64 test used a normal MSI `/i` over an installed predecessor, not a forced reinstall or pre-uninstall. The first MSI run required reboot (`3010`).
- Observed installed processes and a user-reported smoke test do not constitute a complete automated Windows x64 uninstall/rollback or network security audit.
- The old x64 installed MSI runtime hash in this upgrade differs from the tracked `b10437` executable used in the paired quality comparison; these are distinct baselines.
- For future versions, create a new validation report and retain this record for comparison.
