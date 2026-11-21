# How to build Mozc on macOS

<!-- disableFinding(LINK_RELATIVE_G3DOC) -->

[![macOS](https://github.com/kazormia296/mozkey-ibg/actions/workflows/macos.yaml/badge.svg)](https://github.com/kazormia296/mozkey-ibg/actions/workflows/macos.yaml)

## Summary

If you are not sure what the following commands do, please check the
descriptions below and make sure the operations before running them.

```
git clone https://github.com/kazormia296/mozkey-ibg.git
cd mozkey-ibg/src

python3 build_tools/update_deps.py

# CMake is required to build both Qt and the bundled Zenz runtime.
python3 build_tools/build_qt.py --release --confirm_license --macos_cpus=arm64

# Build and stage the source-pinned Universal Zenz runtime (llama.cpp v0.7.0).
RUNTIME="$(mktemp -d)/mozkey-ibg-zenz-runtime"
MOZKEY_ZENZ_MODEL_SOURCE="$PWD/win32/installer/zenz_runtime/models/zenz-v3.2-small-Q5_K_M.gguf" \
  mac/installer/zenz_runtime/build_universal_runtime.zsh "$RUNTIME"
mac/installer/zenz_runtime/stage_runtime_assets.zsh "$RUNTIME"

bazelisk build package --config release_build --macos_cpus=arm64 \
  --define=macos_zenz_runtime=1 --define=MOZKEY_INSTALLABLE=1
open bazel-bin/mac/MozkeyIbG.pkg
```

💡 Mozkey IbG for macOS is distributed for Apple Silicon only: the application
and installer are arm64-only. Since the sync to upstream Mozkey v1.0.0 the
bundled Zenz runtime helpers (`mozc_zenz_scorer`, `mozc_zenz_guard`, and
`llama-server`) are upstream's Universal binaries under
`MozkeyIbG.app/Contents/Resources/ZenzRuntime`.

💡 You can also download `MozkeyIbG.pkg` from GitHub Actions. Check
[Build with GitHub Actions](#build-with-github-actions) for details.

## Setup

### System Requirements

Apple Silicon Macs running macOS 12 or later are supported.

### Software Requirements

Building on Mac requires the following software.

*   [Xcode](https://apps.apple.com/us/app/xcode/id497799835)
    *   Xcode 16.0 or later
    *   ⚠️Xcode Command Line Tools aren't sufficient.
*   [Bazelisk](https://github.com/bazelbuild/bazelisk)
    *   Bazelisk is a wrapper of [Bazel](https://bazel.build/) to use the
        specific version of Bazel.
    *   [src/.bazeliskrc](../src/.bazeliskrc) controls which version of Bazel is
        used.
*   Python 3.12 or later.
*   CMake 3.18.4 or later (to build Qt6 and the pinned Zenz runtime)

## Get the Code

You can download Mozc source code as follows:

```
git clone https://github.com/kazormia296/mozkey-ibg.git
cd mozkey-ibg/src
```

Hereafter you can do all the operations without changing directory.

### Check out additional build dependencies

```
python build_tools/update_deps.py
```

In this step, additional build dependencies will be downloaded.

*   [Ninja 1.11.0](https://github.com/ninja-build/ninja/releases/download/v1.11.0/ninja-mac.zip)
*   [Qt 6.9.1](https://download.qt.io/archive/qt/6.8/6.8.0/submodules/qtbase-everywhere-src-6.9.1.tar.xz)

You can specify `--noqt` option if you would like to use your own Qt binaries.

### Build Qt

```
python3 build_tools/build_qt.py --release --confirm_license --macos_cpus=arm64
```

Drop `--confirm_license` option if you would like to manually confirm the Qt
license.

You can also specify `--debug` option to build debug version of Mozc.

```
python3 build_tools/build_qt.py --release --debug --confirm_license --macos_cpus=arm64
```

The explicit `--macos_cpus=arm64` option matches the Mozkey release contract.

You can skip this process if you have already installed Qt prebuilt binaries.

CMake is required by the Zenz runtime build step even when you use
prebuilt Qt binaries. If you use `brew`, you can install `cmake` as follows.

```
brew install cmake
```

--------------------------------------------------------------------------------

## Build with Bazel

### Build installer

First build and stage the source-pinned Zenz runtime. The builder clones
llama.cpp `v0.7.0` at the pinned commit, applies the tracked tokenizer and
dual-liveness patches, builds `llama-server` for arm64 and x86_64 with a
macOS 12 deployment target, merges them into one Universal executable, and
writes `BUILD-CONTRACT.txt`. `stage_runtime_assets.zsh` re-verifies that
contract and copies the ignored runtime files next to
`mac/installer/zenz_runtime/BUILD.bazel`. See
[the runtime README](../src/mac/installer/zenz_runtime/README.md) for the
contract details.

```
RUNTIME="$(mktemp -d)/mozkey-ibg-zenz-runtime"
MOZKEY_ZENZ_MODEL_SOURCE="$PWD/win32/installer/zenz_runtime/models/zenz-v3.2-small-Q5_K_M.gguf" \
  mac/installer/zenz_runtime/build_universal_runtime.zsh "$RUNTIME"
mac/installer/zenz_runtime/stage_runtime_assets.zsh "$RUNTIME"

bazelisk build package --config release_build --macos_cpus=arm64 \
  --define=macos_zenz_runtime=1 --define=MOZKEY_INSTALLABLE=1
open bazel-bin/mac/MozkeyIbG.pkg
```

*   `--define=macos_zenz_runtime=1` packages the staged `llama-server`, model,
    and `BUILD-CONTRACT.txt`. Without it the package build fails closed in
    `mac/tweak_installer_files.py`, because the Zenz runtime would be
    incomplete.
*   `--define=MOZKEY_INSTALLABLE=1` selects the release preinstall script
    (`mac/preflight_mozkey_release.sh`). Without it the package embeds the
    stage-1 gate and refuses to install.
*   The application keeps the explicit `--macos_cpus=arm64` contract; the Zenz
    runtime helpers are Universal regardless of that flag.

To verify a built package, including a real inference request through the
packaged scorer:

```
MOZKEY_ZENZ_VERIFY_APP_ARCHITECTURES=arm64 \
  mac/installer/zenz_runtime/verify_release_package.zsh bazel-bin/mac/MozkeyIbG.pkg
```

### Unit tests

```
bazelisk test ... --build_tests_only -c dbg
```

See [build Mozc in Docker](build_mozc_in_docker.md#unittests) for details.

### Mozkey Grimodex desktop contract tests

The macOS workflow also runs the portable Protocol v1 fixtures, the secure
POSIX reader/registrar tests, IMKit context and renderer callback tests, and the
packaged Zenz runtime probes. See
[Grimodex Protocol v1 on Windows and macOS](grimodex_desktop_integration.md)
for the focused command and the security/lifecycle contract.

### Edit src/config.bzl

You can modify variables in `src/config.bzl` to fit your environment. Note: `~`
does not represent the home directory. The exact path should be specified (e.g.
`MACOS_QT_PATH = "/Users/mozc/myqt"`).

Tips: the following command makes the specified file untracked by Git.

```
git update-index --assume-unchanged src/config.bzl
```

This command reverts the above change.

```
git update-index --no-assume-unchanged src/config.bzl
```

--------------------------------------------------------------------------------

## Build with GitHub Actions

GitHub Actions steps are already set up in
[macos.yaml](../.github/workflows/macos.yaml). Pull requests and ordinary pushes
run the macOS test gate only; they do not build or upload a package. Installer
builds are called by the release workflow from a validated release tag.

1.  Fork https://github.com/kazormia296/mozkey-ibg to your GitHub repository.
2.  Update `src/version.bzl`, merge the tested change into `main`, and create an
    annotated `vX.Y.Z` tag with the same version.
3.  Push the tag and wait for the **Mozkey Release** workflow to succeed.
4.  Review the generated draft prerelease and its checksums.
5.  Download `MozkeyIbG_vX.Y.Z_macos_arm64.pkg` after publishing the release.

See [Releasing Mozkey](releasing.md) for the exact version, ancestry, release
notes, and rerun contract.

You can also find Mozkey installers for macOS in the Mozkey repository. Please
keep in mind that Mozc is not an officially supported Google product, even if
downloaded from https://github.com/kazormia296/mozkey-ibg/.

1.  Sign in GitHub.
2.  Open the repository's
    [Releases](https://github.com/kazormia296/mozkey-ibg/releases) page.
3.  Download the macOS arm64 package for the intended version.

--------------------------------------------------------------------------------

## Build with GYP (deprecated):

⚠️ The GYP build is deprecated and no longer supported.

Please check the previous version for more information.
https://github.com/google/mozc/blob/3.33.6089/docs/build_mozc_in_osx.md#build-with-gyp-maintenance-mode
