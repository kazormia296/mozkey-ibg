# Linux product isolation

Mozkey IbG's OSS Linux build is intentionally installable alongside both a
distribution Mozc package and an upstream Mozkey installation. The three
products do not share an Fcitx or IBus identity, profile, server endpoint,
named event, or installed executable directory. Mozkey IbG never reads or
migrates a Mozc or upstream Mozkey profile.

| Resource | Mozkey path or identity |
| --- | --- |
| Fcitx addon | `mozkey-ibg`, `${fcitx5_libdir}/fcitx5/fcitx5-mozkey-ibg.so` |
| Fcitx input method | `mozkey-ibg` |
| Profile | `$XDG_CONFIG_HOME/mozkey-ibg`, otherwise `~/.config/mozkey-ibg` |
| Legacy profile | `~/.mozkey-ibg` |
| Linux abstract IPC prefix | `/tmp/.mozkey-ibg.` (the leading slash becomes NUL) |
| Named event prefix | `mozkey-ibg.event.` |
| Server and tools | `/usr/lib/mozkey-ibg` |
| Zenz socket and lock | `~/.mozkey-ibg_zenz_scorer_pipe`, `~/.mozkey-ibg_zenz_scorer.lock` |
| Zenz runtime | Arch/source: link to distro `/usr/bin/llama-server`; deb/rpm: attested regular file at `/usr/lib/mozkey-ibg/llama-server` |
| Zenz model | `/usr/lib/mozkey-ibg/models/zenz-v3.2-small-Q5_K_M.gguf` |

## Relationship to upstream Mozkey's Linux isolation

Upstream Mozkey 1.1.0 isolates its own Linux build from Mozc. Mozkey IbG
adopts the same mechanism (the Linux-desktop-only `kEventPathPrefix` and
`kIPCPrefix` branches, the `LINUX_MOZC_*`/`IBUS_MOZC_*` values in
`src/config.bzl`, the strict `src/unix/build_package.py` input manifest, the
`--branding=Mozkey` IBus component generator, and upstream's
`//ipc:linux_mozkey_namespace_test`), but every resulting identifier is the
Mozkey IbG one:

| Resource | Stock Mozc | Upstream Mozkey 1.1.0 | Mozkey IbG |
| --- | --- | --- | --- |
| Server and tools | `/usr/lib/mozc` | `/usr/lib/mozkey` | `/usr/lib/mozkey-ibg` |
| Icons | `/usr/share/icons/mozc` | `/usr/share/icons/mozkey` | `/usr/share/icons/mozkey-ibg` |
| XDG profile | `mozc` | `mozkey` | `mozkey-ibg` |
| Legacy home profile | `~/.mozc` (adopted if present) | none | `~/.mozkey-ibg` (adopted if present) |
| Named event prefix | `mozc.event.` | `mozkey.event.` | `mozkey-ibg.event.` |
| Abstract IPC prefix | `/tmp/.mozc.` | `/tmp/.mozkey.` | `/tmp/.mozkey-ibg.` |
| IBus component file | `mozc.xml` | `mozkey.xml` | `mozkey-ibg.xml` |
| IBus component name | `com.google.IBus.Mozc` | `org.mozkey.IBus.Mozkey` | `io.github.kazormia296.IBus.MozkeyIbG` |
| IBus engine binary | `/usr/lib/ibus-mozc/ibus-engine-mozc` | `/usr/lib/ibus-mozkey/ibus-engine-mozkey` | `/usr/lib/ibus-mozkey-ibg/ibus-engine-mozkey-ibg` |
| IBus icons and text domain | `/usr/share/ibus-mozc`, `ibus-mozc` | `/usr/share/ibus-mozkey`, `ibus-mozkey` | `/usr/share/ibus-mozkey-ibg`, `ibus-mozkey-ibg` |
| IBus engine names | `mozc-jp`, `mozc-on`, `mozc-off` | `mozkey-jp`, `mozkey-on`, `mozkey-off` | `mozkey-ibg-jp`, `mozkey-ibg-on`, `mozkey-ibg-off` |

The one deliberate difference from upstream is the legacy home profile:
upstream Mozkey has none, while Mozkey IbG keeps honouring its own
pre-existing `~/.mozkey-ibg`. Neither `~/.mozc`, `~/.mozkey`,
`$XDG_CONFIG_HOME/mozc` nor `$XDG_CONFIG_HOME/mozkey` is ever adopted, and no
history or user dictionary is migrated from them.

The released Linux packages ship the Fcitx5 frontend only. The IBus frontend
is still buildable from source through `//unix:package`; the install scripts,
deb/rpm/Arch packaging and the install manifest do not contain it. The rows
above keep such a source build from replacing a Mozc or upstream Mozkey IBus
component. The Emacs helper is not part of `//unix:package`.

`tools/release/product_identity_contract_test.py` pins these values so that a
later upstream merge cannot silently reintroduce the `mozkey` names.

## Grimodex scope

Grimodex project data is application-scoped independently of these product
paths. The default `MOZKEY_GRIMODEX_SCOPE` is `grimodex-only`: only exact,
case-insensitive `grimodex` and `com.miyakey.grimodex` program identities may
pin the project dictionary or its Zenz context. Empty/unknown identities fail
closed. `MOZKEY_GRIMODEX_SCOPE=all` is the explicit all-applications opt-in;
`off` and unknown non-empty values disable project integration without
disabling ordinary Mozkey conversion or learning.

`/usr/lib/mozkey-ibg` is used as the product-private libexec directory because the
server, GUI tool, and Zenz scorer are implementation helpers launched by the
frontend and are not general-purpose commands for `$PATH`. The directory also
matches `SystemUtil::GetServerDirectory()` in the OSS Linux Bazel build.

The Fcitx addon directory is distribution-specific. Source installation asks
`pkg-config --variable=libdir Fcitx5Core` and appends `/fcitx5`; Debian/Ubuntu
multiarch paths and Fedora `/usr/lib64` therefore do not fall back to the Arch
path. Packagers may set an absolute `FCITX5_ADDON_DIR`, but it must remain under
a validated `/usr` library directory. The resolved path is installed as
`/usr/share/mozkey-ibg/fcitx5-addon-dir`, consumed by E2E and uninstall, and audited
by the staging smoke. `PREFIX` is intentionally fixed to `/usr` because the
OSS binary embeds `/usr/lib/mozkey-ibg`; only `DESTDIR` is relocatable for package
staging.

From the repository root, a fresh clone can build the exact Arch release target
in one command. Debian and Fedora first build the pinned native runtime that is
included in their attestation. Missing ignored dictionary outputs are generated
by the pinned Linux-native pipeline before Bazel starts:

```sh
scripts/build_mozkey_linux_bazel archlinux-x86_64

python3 tools/release/prepare_linux_zenz_runtime.py
scripts/build_mozkey_linux_bazel ubuntu-layout
scripts/build_mozkey_linux_bazel fedora-x86_64
```

Each target command must run on its matching distribution. Arch source
installation uses the distribution `llama-cpp` runtime; after its release
targets are built, run from `src/`:

```sh
../scripts/preflight_mozkey_linux_bazel
../scripts/smoke_test_mozkey_fcitx5_install
sudo env PREFIX=/usr ../scripts/install_mozkey_linux_bazel
MOZKEY_LINUX_OUTPUT_DIR="$(pwd)/../dist" \
  ../scripts/package_mozkey_linux_bazel
```

The combined source installer performs a no-write preflight, a full
temporary-stage smoke, installs metadata, then the server/runtime set, and
atomically publishes the Fcitx entry point last. Individual Bazel install
scripts are internal primitives. The smoke also covers a changed multiarch
addon path and rejects an unsafe installed path record before the data phase.

Release binaries must be produced by `scripts/build_mozkey_linux_bazel`. Its
attestation binds the current Git HEAD, exact Bazel targets and flags,
`release-approved-only` manifest/lock/outputs, every installed ELF digest, and
the Zenz source/normalization-lock/derived-model/tensor-payload digests.
Preflight and packaging reject stale or differently configured `bazel-bin`
outputs. Every Linux package and SPDX inventory also contains that attestation
and the recorded versions of selected native toolchain/runtime packages
(`dpkg-query`, `rpm -q`, or `pacman -Q`), so the declared build environment is
not merely an unauthenticated CI sidecar. The Debian and RPM attestations
additionally bind the digest and size of their bundled llama.cpp runtime.

The wrapper accepts a verified CI dictionary transfer as a fast path. On a
fresh clone it instead downloads only source-lock-pinned inputs, verifies the
dated jawiki build-statistics payload and the deterministic raw merge digest,
generates the three-file public allowlist without PowerShell, then rechecks the
clean tracked worktree before compiling.

Release CI builds three Linux products natively on x86_64:

- Ubuntu 24.04 emits `mozkey-ibg_X.Y.Z_amd64.deb` and validates the Debian
  multiarch Fcitx addon path.
- Fedora 44 emits `mozkey-ibg-X.Y.Z-1.x86_64.rpm` and validates the Fedora
  `/usr/lib64` addon path.
- Arch Linux emits a verified `.tar.xz` payload, basename-scoped SHA-256
  sidecar, and SPDX 2.3 inventory. The published payload backs the
  `mozkey-ibg-bin` AUR package.

The deb and RPM contain a separately attested, pinned llama.cpp b9637
`llama-server` built without shared llama/ggml libraries, network clients,
hardware-specific CPU tuning, or extended x86 instruction-set requirements.
The Arch payload instead requires compatible `fcitx5`, `llama-cpp`, and
`qt6-base` distribution packages. Each job loads the real GGUF through the
scorer, exercises authenticated `/completion`, and records the native package
versions used to build it. AppImage is intentionally not emitted because an
Fcitx5 IME must install system addon and input-method metadata.

Ordinary pull requests and `main` pushes run the Linux dictionary and test gate
without building product payloads. The Ubuntu/deb, Fedora/RPM, and verified Arch
payload jobs run only from the release workflow after a `vX.Y.Z` tag passes the
version and `main` ancestry checks. After the one-time AUR key/environment setup,
publishing the reviewed GitHub Release updates AUR; drafts are rejected. See
[Releasing Mozkey](releasing.md).

Public Linux builds also pass
`--define=mozkey_dictionary_profile=release-approved-only`. The selected
dictionary target contains only pinned merge-ut place-name/SudachiDict data,
pinned personal names, and the Mozkey syntax guard. Nico/Pixiv stays available
to the separately named local-evaluation profile but is absent from the CI
dictionary transfer artifact and compiled public product. The installed
dictionary notice, Apache-2.0 text, and immutable source lock live under
`/usr/share/licenses/mozkey-ibg/dictionary/`.

Arch and source packaging depend on a compatible `llama-server` provider. The
default private link targets `/usr/bin/llama-server`; a source packager with a
different absolute path can set `MOZKEY_ZENZ_LLAMA_SERVER_TARGET` while
staging. Debian and RPM packaging instead copy the pinned, attested runtime into
that product-private path as a regular executable and reject any digest drift.
The tracked source GGUF uses a private tokenizer metadata name and incorrect
special-token IDs, so `scripts/build_mozkey_linux_bazel` generates a pinned
Linux derivative before Bazel. The transformation changes only four GGUF
metadata values; tensor descriptors and tensor data are byte-identical.
Preflight, attestation, and installation independently verify the fixed source
SHA-256, normalized SHA-256, tensor-payload SHA-256, and deterministic output.
The resulting GGUF and its notices are installed from that verified build
state, so release mode never depends on an environment override or a patched
llama.cpp fork.
The supported runtime contract is the `-m`/`--model`, `-c`/`--ctx-size`,
`-t`/`--threads`, `--host`, `--port`, and `--api-key` CLI plus authenticated
`POST /completion` returning a JSON `content` field. A real source install runs
the bounded CLI compatibility check; native packages verify the attested
runtime before staging. Release CI and dogfood additionally load the bundled
model and exercise `/completion` while checking that the child listener is
loopback-only. The 2026-07-17 host dogfood gate uses llama.cpp build 9859
(`4fc4ec5541`); the Arch CI payload records its exact rolling package version,
while deb/rpm record the pinned b9637 runtime digest and build options.
Prompt construction follows the model's reference tokenizer preparation:
ASCII spaces become U+3000 and line feeds are removed before inference. This
keeps the Windows source model and Linux normalized model on one prompt
contract even though their llama.cpp integration differs.

`zenzai_v3_conditions` in the consumer heartbeat means the installed product
has an executable scorer, immutable non-empty GGUF, and executable llama
runtime. It is false for a partial install. It does not claim that the large
model is already loaded; endpoint readiness is proved by the separate Zenz
runtime probe.

The smoke test installs into a temporary `DESTDIR`, validates the Fcitx and
AppStream metadata, repeats the install as an upgrade, removes only the audited
Mozkey manifest, proves Hazkey/Mozc/user-data sentinels survive, and reinstalls
the product. For a live uninstall, first stop Fcitx so it cannot respawn the
per-user server, then run the bounded runtime gate as that desktop user while
the installed executable paths still exist:

```sh
fcitx5-remote -e
./scripts/stop_mozkey_linux_runtime
```

The stop gate uses Linux pidfds and repeated `/proc` identity checks. It can
signal only the exact `/usr/lib/mozkey-ibg/mozc_server` and
`/usr/lib/mozkey-ibg/mozc_zenz_scorer` processes belonging to the target UID, plus
an exact `/usr/bin/llama-server` (Arch/source install) or
`/usr/lib/mozkey-ibg/llama-server` (deb/rpm) proven to descend from that scorer. It
never falls back to a basename or command-line match. Root automation must name
the non-root desktop UID explicitly with `--uid`; `DESTDIR` and non-`/usr`
layouts are rejected. Only after that gate succeeds should the product files be
removed:

```sh
sudo env PREFIX=/usr ./scripts/uninstall_mozkey_fcitx5
```

It deliberately preserves `$XDG_CONFIG_HOME/mozkey-ibg`, `~/.config/mozkey-ibg`, the
legacy profile, per-user Zenz feedback, and Grimodex Protocol v1 snapshots.
The system-owned bundled GGUF is a product file and is removed. Windows and
macOS product paths are unchanged.

To remove only this product's consumer heartbeat during a root-owned package
uninstall, pass the user's absolute Protocol v1 root explicitly:

```sh
sudo env PREFIX=/usr MOZKEY_REMOVE_GRIMODEX_CONSUMER=1 \
  GRIMODEX_IME_ROOT="$HOME/.local/share/com.miyakey.grimodex/ime" \
  ./scripts/uninstall_mozkey_fcitx5
```

The uninstaller resolves the root owner before removing the runtime marker and
uses `setpriv` to run the native unregister helper as that owner.  The helper
still performs its fd-relative ownership, mode, and symlink checks; it removes
only `consumers/fcitx5-mozkey-ibg.json`.

## Real Fcitx5 consumer E2E

Grimodex's ignored Linux process test must use the installed addon rather than
launch `mozc_server` directly. After installing the Mozkey Fcitx5 and server
products, point `GRIMODEX_LINUX_MOZKEY_LAUNCHER` at
`scripts/launch_fcitx5_mozkey_e2e` in this checkout. The launcher preserves the
test-provided `GRIMODEX_IME_ROOT`, `HOME`, and XDG homes, installs a private
Mozkey-only Fcitx profile, and starts foreground Fcitx5 on a new D-Bus session.
Detection of `consumers/fcitx5-mozkey-ibg.json` therefore proves the actual addon
was loaded and executed its startup heartbeat. This is intentionally a
heartbeat-only process gate. Native session fault injection separately proves
snapshot reload, security-domain purge, stale-candidate rejection and server
restart recovery; real toolkit dogfood proves key/preedit/candidate behavior.
