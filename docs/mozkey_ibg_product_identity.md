# Mozkey IbG product identity

Mozkey IbG is installed and registered as a product distinct from the upstream
Mozkey fork. The public slug is `mozkey-ibg`, the logical prefix is
`MozkeyIbG`, and the user-facing short name is **Mozkey IbG**.

This identity split intentionally does not migrate profiles, registrations, or
installation state from earlier development builds. Mozkey IbG has not had a
formal release, so installers only own the new identifiers listed here.

## Cross-platform contract

| Surface | Linux | Windows | macOS |
| --- | --- | --- | --- |
| Release asset | `mozkey-ibg-vX.Y.Z-*`, `mozkey-ibg_*`, `mozkey-ibg-*` | `MozkeyIbG_vX.Y.Z_*.msi` | `MozkeyIbG_vX.Y.Z_macos_arm64.pkg` |
| Install root | `/usr/lib/mozkey-ibg` | `Program Files (x86)/MozkeyIbG` | `/Library/Input Methods/MozkeyIbG.app` |
| User profile | `$XDG_CONFIG_HOME/mozkey-ibg` or `~/.mozkey-ibg` | `%LOCALAPPDATA%/Mozkey IbG` and `AppData/LocalLow/MozkeyIbG` feedback | `Application Support/MozkeyIbG` |
| Grimodex consumer | `fcitx5-mozkey-ibg` | `tsf-mozkey-ibg` | `imkit-mozkey-ibg` |
| Runtime namespace | `mozkey-ibg.event.*`, `/tmp/.mozkey-ibg.*`, `.mozkey-ibg_zenz_*` | `Local\\MozkeyIbG.*`, `\\\\.\\pipe\\mozkey-ibg.*`, `\\\\.\\pipe\\mozkey-ibg_zenz_scorer` | `MozkeyIbG.event.*`, `io.github.kazormia296.mozkey-ibg.*`, `.mozkey-ibg_zenz_*` |

## Linux

- Package names: `mozkey-ibg`, `mozkey-ibg-bin`.
- Fcitx addon/input-method ID: `mozkey-ibg`.
- Fcitx shared library: `fcitx5-mozkey-ibg.so`.
- Fcitx metadata: `mozkey-ibg.conf`, `mozkey-ibg-addon.conf`.
- AppStream ID: `io.github.kazormia296.MozkeyIbG`.
- Product data, licenses, icons, and bookkeeping use `mozkey-ibg` paths.
- Profile: `$XDG_CONFIG_HOME/mozkey-ibg`, otherwise `~/.config/mozkey-ibg`; an
  existing `~/.mozkey-ibg` is honoured first.
- Named events: `mozkey-ibg.event.*`. Abstract IPC sockets: `/tmp/.mozkey-ibg.*`.
- Optional IBus frontend (`//unix:package`, not part of the released Linux
  packages): component file `/usr/share/ibus/component/mozkey-ibg.xml`,
  component name `io.github.kazormia296.IBus.MozkeyIbG`, engine binary
  `/usr/lib/ibus-mozkey-ibg/ibus-engine-mozkey-ibg`, icons
  `/usr/share/ibus-mozkey-ibg` and `/usr/share/icons/mozkey-ibg`, text domain
  `ibus-mozkey-ibg`, engine names `mozkey-ibg-jp`, `mozkey-ibg-on`,
  `mozkey-ibg-off`.

The installed executable basenames remain `mozc_server`, `mozc_tool`, and
related upstream-compatible names. They live under `/usr/lib/mozkey-ibg`, and
their profile, event, socket, and IPC namespaces are distinct, so the basenames
do not prevent concurrent operation.

Upstream Mozkey 1.1.0 isolates its own Linux build from Mozc under `mozkey`
names (`/usr/lib/mozkey`, `~/.config/mozkey`, `mozkey.event.*`,
`/tmp/.mozkey.*`, `org.mozkey.IBus.Mozkey`, `mozkey-jp`). Mozkey IbG uses the
same mechanism and build structure with the `mozkey-ibg` identifiers above, so
it shares nothing with either upstream Mozkey or distribution Mozc. See
[Linux product isolation](linux_product_isolation.md).

## Windows

- MSI UpgradeCode: `{422E6070-C76C-4F9B-96BE-FD9569E4B762}`.
- TSF text service: `{2D046FEA-2B23-4E77-946B-FC2AF48219DC}`.
- TSF language profile: `{A5F4AF8E-7338-4A5C-9186-FF5B05B28393}`.
- Service name: `MozkeyIbGCacheService`.
- Registry root: `Software\\Grimodex\\MozkeyIbG`.
- Startup entry: `Mozkey IbG Prelauncher`.
- Firewall rules: `Mozkey IbG Offline - ...`.
- Renderer callback messages: `mozkeyibg.renderer.message`,
  `mozkeyibg.renderer.highlight.message`, `mozkeyibg.renderer.submit.message`.
- Default-IME override backup: `HKCU\Software\Grimodex\MozkeyIbG\DefaultImeOverrideBackup`.

All TSF auxiliary GUIDs are also fork-specific. Windows executable basenames
remain upstream-compatible, but they are installed below `Program Files (x86)/MozkeyIbG`
and every registration, service, singleton, IPC endpoint, window class, and
uninstall action resolves the Mozkey IbG product path. The MSI does not own the
shared Terminal Server `SysProcs` values keyed only by an executable basename.

## macOS

- Main bundle: `/Library/Input Methods/MozkeyIbG.app`.
- Bundle namespace: `io.github.kazormia296.mozkey-ibg.inputmethod.Japanese`.
- Package receipt: `io.github.kazormia296.mozkey-ibg.pkg.JapaneseInput`.
- Helper apps: `MozkeyIbGConverter`, `MozkeyIbGRenderer`, `MozkeyIbGTool`, and
  `MozkeyIbGPrelauncher`.
- LaunchAgent labels and filenames use
  `io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.*`.

The uninstaller removes only these Mozkey IbG paths and labels. It does not
remove upstream Mozkey bundles, launch agents, profiles, or Grimodex consumers.

The bundled Zenz runtime lives in `MozkeyIbG.app/Contents/Resources/ZenzRuntime`
and its scorer socket and lock live in `/tmp/mozkey-ibg_zenz_<uid>/`, never in
upstream Mozkey's `/tmp/mozc_zenz_<uid>/`.

The package preinstall (`src/mac/preflight_mozkey_release.sh`) and postinstall
(`src/mac/postflight_mozkey_oss_cleanup.sh`, appended by
`src/mac/postflight_mozkey_oss_filter.py`) gate on, verify and signal Mozkey IbG
only. The scorer is stopped by its full path under `MozkeyIbG.app`, and the
generic `killall llama-server` is filtered out, so an upstream runtime is not
signalled. `//mac:tweak_preflight` and `//mac:tweak_postflight` fail the build
if a generated script names `org.mozc.*`, `Mozc.app` or `/Applications/Mozc`.
The receipt and `CFBundleVersion` use the installer version `100 + major`
(`100.10.4` for v0.10.4); packages up to v0.10.4 left receipt version `0` and
`3.33.x` bundles, which the preinstall accepts as the legacy upgrade path; it
then removes that verified legacy Mozkey IbG bundle so its old in-bundle Zenz
runtime cannot survive as unsealed files. A
release package must be built with `--define=MOZKEY_INSTALLABLE=1`; the default
package embeds a preinstall that refuses to install.

## Co-installation rule

Installing or uninstalling Mozkey IbG must not create, replace, unregister, or
delete an upstream Mozkey product identifier. A contract test checks the
platform identifiers and rejects the known upstream registration values in the
live Mozkey IbG packaging/runtime surfaces.
