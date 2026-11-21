# Secure Offline Guarantee

## Goal

This fork is designed to operate as an offline Japanese IME.

Mozc runtime processes should not initiate Internet communication during normal
IME operation.

## Runtime network policy

Mozc runtime binaries must not initiate outbound Internet communication.

This includes:

- cloud conversion
- usage statistics upload
- crash report upload
- auto-update
- online dictionary update
- online suggestion service
- remote configuration fetch
- telemetry
- opening external URLs from Mozc runtime processes

## Local-only design

This fork uses local resources for normal IME operation:

- local system dictionary
- local user dictionary
- local user history
- local settings
- a local Zenz scorer and GGUF model when Zenz support is included
- bundled Windows and macOS llama.cpp runtimes, and either an attested bundled
  Linux runtime (deb/rpm) or a verified Linux distribution runtime (Arch/source)

Input text is processed locally.

### Windows Zenz path

Windows Zenz builds use `mozc_zenz_scorer.exe`, the bundled
`llama-server.exe`, and the bundled local GGUF model. The scorer communicates
with the runtime through localhost only.

### macOS Zenz path

Since the sync to upstream Mozkey v1.0.0, macOS packages contain upstream
Mozkey's runtime under `MozkeyIbG.app/Contents/Resources/ZenzRuntime` inside
the signed package payload: the Universal `mozc_zenz_scorer` and
`mozc_zenz_guard` helpers, the source-pinned and patched Universal
`llama-server` (llama.cpp v0.7.0), the unmodified GGUF model, and
`BUILD-CONTRACT.txt`. The converter talks to the scorer over a per-user Unix
domain socket in a Mozkey IbG-specific directory
(`/tmp/mozkey-ibg_zenz_<uid>`), never over the network, and the scorer
communicates with `llama-server` through localhost only. `mozc_zenz_guard`
supervises the worker so it exits when the scorer goes away. Release CI
verifies the embedded build contract and completes a real inference request
through the packaged scorer before signing
(`src/mac/installer/zenz_runtime/verify_release_package.zsh`).

### Linux Zenz path

Linux Zenz builds use `mozc_zenz_scorer`, the bundled local GGUF model, and the
product-private `/usr/lib/mozkey-ibg/llama-server` path. Arch/source installs link
that path to a compatible distribution runtime. Debian and RPM packages contain
an attested, pinned CPU-only llama.cpp runtime as a regular executable.
Installation or staging checks the applicable runtime's identity and CLI
contract, while release dogfood must also load the model and complete an
authenticated inference probe.
The Linux GGUF is a deterministic metadata-only derivative of the pinned
source model: build attestation verifies both complete-file digests and the
unchanged tensor-data digest, so no patched llama.cpp fork is required.
Consumer heartbeat capability reports install completeness, not endpoint or
model readiness.

### Shared localhost boundary

On all three desktop platforms, the scorer-to-runtime transport is
localhost-only and must be bound to `127.0.0.1`.

The Zenz localhost transport must not rely on a fixed public-facing endpoint.
The platform scorer chooses a random high port, reserves that loopback port
until `llama-server` is created, and verifies the child executable image before
accepting it. It protects completion requests with a random API key. Generated
port and API key values must not be written to release logs.

The API key is a defense-in-depth measure for stale, accidental, or mismatched
localhost endpoints. It is passed to the platform `llama-server` through the
command line because that is the interface exposed by llama.cpp server.
Therefore it should not be treated as a strong secret against same-user local
malware. The primary boundary is that the server is bound to `127.0.0.1`, uses
a random high port, and is launched and managed by the platform scorer. The
same-user residual risk is explicit: because the bundled llama.cpp interface
accepts `--api-key` on its command line, same-user malware with process
inspection privileges may still observe that defense-in-depth key. Such a
process is outside this desktop user's threat boundary; the scorer's private
IPC endpoint, loopback-only listener, port reservation, and child-image check
remain mandatory controls.

## macOS offline verification

For macOS Zenz-bundled builds, Mozc communicates with its scorer through a
private, per-user Unix-domain socket. The scorer-to-`llama-server` connection
is authenticated and bound to randomized localhost (`127.0.0.1`) transport.

The macOS-specific checks include:

- `python3 tools/check_macos_offline_source.py` for selected macOS/shared
  runtime source contracts;
- `python3 -m unittest discover -s tools/tests -p 'test_macos_offline_audit.py' -v`
  for the guard's regression cases;
- `src/mac/installer/zenz_runtime/verify_release_package.zsh` for the final
  unpacked PKG, including a native scorer inference probe and sampled `ps` /
  `lsof` socket audit of the scorer and descendants.

The sampled socket audit records no prompts or model outputs and rejects
observed non-loopback IP endpoints. It is **not an OS-enforced egress block**,
may miss short-lived connections, and does not cover every installed Mozkey IbG
process. Native Intel verification and installed full-IME audits are separate
release requirements. Build-time dependency/model downloads are not treated
as installed-IME runtime communications.

See [macOS offline audit contract](macos_offline_audit.md) for the detailed
scope and limitations.

## Usage statistics and crash reports

The usage-statistics and crash-report option inherited from upstream is removed
from the administration and configuration dialogs.

The default `StatsConfigUtil` implementation is fixed to the null implementation
in this fork. Usage statistics cannot be enabled through the normal runtime
path.

## Source checks

Obvious runtime networking APIs are rejected by:

```powershell
python tools\check_no_network_symbols.py
```

## Binary import checks

Windows release binaries should be checked so that Mozc core runtime executables
do not import common networking DLLs such as:

- ws2_32.dll
- winhttp.dll
- wininet.dll
- urlmon.dll
- webio.dll
- dnsapi.dll

`mozc_zenz_scorer.exe` is a narrow local-only exception. It may import
`winhttp.dll` to request inference from the bundled `llama-server.exe` on
`127.0.0.1`, and `ws2_32.dll` to bind `127.0.0.1:0` temporarily for selecting
a non-fixed loopback port. These exceptions apply only to the scorer; all
other prohibited DLLs remain denied, including those imported by other Mozc
runtime binaries. The scorer must not contact external hosts, accept a
release-build environment override for runtime/model paths, or expose the
inference endpoint externally. DLL import checks alone cannot prove this:
installed-MSI loopback and connection audits remain required (see the
release checklist).

The check is implemented by:

```powershell
python tools\check_no_network_imports.py --root src\bazel-bin
```

The pull-request `Secure Offline Checks` workflow builds
`mozc_zenz_scorer.exe`, prepares the pinned x64 `llama-server.exe`, and checks
those explicit binaries without rebuilding a complete installer. For release
validation, each Windows package job checks the Bazel output, bundled runtime,
and MSI-extracted payload before it uploads that exact MSI.

## Binary string checks

Windows release binaries should be checked so that Mozc runtime executables do
not contain hard-deny telemetry, updater, crash-upload, or usage-statistics
markers.

Generic URL-like markers such as `http://`, `https://`, `googleapis.com`, and
protobuf field names such as `usage_stats` are reported for audit. They are not
treated as hard failures by themselves.

The check is implemented by:

```powershell
python tools\check_no_network_strings.py --root src\bazel-bin
```

The same PR and release split applies to string checks: routine CI inspects the
actual scorer and local runtime, while release CI inspects the exact MSI payload
that will be published.

## Windows Firewall hardening

The Windows installer adds outbound Windows Firewall block rules for Mozc
runtime executables as an additional offline hardening layer.

The rules target Mozc executable files such as:

- mozc_server.exe
- mozc_tool.exe
- mozc_renderer.exe
- mozc_broker.exe
- mozc_cache_service.exe
- mozc_zenz_scorer.exe
- llama-server.exe

The rules are outbound-only. They are removed during uninstall. Firewall rule
creation and removal are best-effort operations; installation and uninstall do
not fail solely because local policy rejects firewall changes.

All seven rules must exist against the actual installed executable paths.
With the rules enabled, the scorer-to-llama localhost path must still work,
scorer-owned `llama-server.exe` must listen on loopback only, and the Zenz
scorer / llama processes must have no non-loopback TCP connection; uninstall
must remove the rules. Upstream Mozkey enforces this with a native ARM64
installed-MSI CI gate. Mozkey IbG CI currently covers the MSI payload
(`tools/release/check_windows_msi_offline.ps1`), the loopback-only listener of
the packaged runtime (`tools/release/probe_windows_zenz_runtime.ps1`), and the
firewall custom-action unit tests; the installed-MSI firewall and connection
audit is not yet ported to the Mozkey IbG runtime and remains a manual
release-checklist item
(`.github/scripts/windows_zenz_arm64_installer_lifecycle_audit.ps1` carries
the Mozkey IbG rule names for that port).

The TIP DLL is not managed by a firewall rule because Windows Firewall program
rules are executable-oriented. Instead, TIP DLL network capability is checked by
source, import, and string audits.

## Runtime checks

Before publishing a release, install the MSI in a clean Windows VM and confirm
that no Mozc core runtime process initiates outbound communication during normal
IME operation.

For Zenz-bundled builds, also confirm that `llama-server.exe` listens only on
`127.0.0.1`, uses a non-fixed random high port, and does not expose an external
interface.

```powershell
Get-Process llama-server -ErrorAction SilentlyContinue |
  ForEach-Object {
    Get-NetTCPConnection -OwningProcess $_.Id -State Listen |
      Select-Object LocalAddress, LocalPort, OwningProcess
  }
```

Expected result:

```text
LocalAddress: 127.0.0.1
LocalPort: not 18080
```

Recommended tools:

- Windows Defender Firewall log
- Resource Monitor
- Process Monitor
- pktmon
- Wireshark

## Build-time network access

Building from source may require network access to download build dependencies.

This is separate from runtime behavior of the installed IME.

## Local data protection

Offline operation means that the IME does not send input data to external
servers.

Local data protection is a separate concern. User dictionary, user history, and
settings are stored on the local machine. Users who need stronger local data
protection should enable OS-level disk encryption such as BitLocker.
