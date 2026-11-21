# How to build Mozc in Windows

<!-- disableFinding(LINK_RELATIVE_G3DOC) -->

[![Windows](https://github.com/kazormia296/mozkey-ibg/actions/workflows/windows.yaml/badge.svg)](https://github.com/kazormia296/mozkey-ibg/actions/workflows/windows.yaml)

## Summary

If you are unsure about what the following commands do, please review the
descriptions below to understand the operations before running them.

```
git clone https://github.com/kazormia296/mozkey-ibg.git
cd mozkey-ibg\src

python build_tools/update_deps.py
python ..\tools\release\prepare_windows_zenz_runtime.py --arch x64
python build_tools/build_qt.py --release --confirm_license
bazelisk build package --config release_build

python build_tools/open.py bazel-bin/win32/installer/Mozc64.msi
```

> [!TIP] You can also download `Mozc64.msi` from GitHub Actions. Check
> [Build with GitHub Actions](#build-with-github-actions) for details.

## Setup

### System Requirements

64-bit Windows 10 or later.

### Software Requirements

Building Mozc on Windows requires the following software.

*   [Visual Studio 2022 Community Edition](https://visualstudio.microsoft.com/downloads/#visual-studio-community-2022)
    with the following components.
    *   Windows 11 SDK
    *   MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)
    *   MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools (Latest)
    *   C++ ATL for latest v143 build tools (x86 & x64)
    *   C++ ATL for latest v143 build tools (ARM64/ARM64EC)
*   Python 3.12 or later.
*   `.NET 6` or later (for `dotnet` command).
*   [Bazelisk](https://github.com/bazelbuild/bazelisk)

> [!TIP] The following Visual Studio components can be skipped if you do not
> build Mozc for ARM64.
>
>  *   MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools (Latest)
>  *   C++ ATL for latest v143 build tools (ARM64/ARM64EC)

> [!TIP] Visual Studio 2026 Community Edition is also supported to build Mozc.
> When both VS 2022 and 2026 are installed, VS 2022 will be used.

> [!NOTE] Bazelisk is a wrapper of [Bazel](https://bazel.build) that allows you
> to use a specific version of Bazel.

### Download the repository from GitHub

```
git clone https://github.com/kazormia296/mozkey-ibg.git
cd mozkey-ibg\src
```

Hereafter you can do all the operations without changing directory.

### Check out additional build dependencies

```
python build_tools/update_deps.py
```

In this step, additional build dependencies will be downloaded, including:

*   [LLVM 22.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8)
*   [MSYS2 2025-02-21](https://github.com/msys2/msys2-installer/releases/tag/2025-02-21)
*   [Ninja 1.13.2](https://github.com/ninja-build/ninja/releases/tag/v1.13.2)
*   [Qt 6.9.1](https://download.qt.io/archive/qt/6.8/6.8.0/submodules/qtbase-everywhere-src-6.9.1.tar.xz)
*   [.NET tools](../dotnet-tools.json)

## Build

### Build Qt

```
python build_tools/build_qt.py --release --confirm_license
```

If you would like to manually confirm the Qt license, omit the
`--confirm_license` option.

### Prepare the pinned Zenz runtime

The MSI never packages the historical root-level llama.cpp PE files. Build and
verify the architecture-specific static runtime from the checksum-pinned
official llama.cpp `b9637` archive (commit
`aedb2a5e9ca3d4064148bbb919e0ddc0c1b70ab3`) first:

```
python ..\tools\release\prepare_windows_zenz_runtime.py --arch x64
python ..\tools\release\prepare_windows_zenz_runtime.py --arch x64 --verify-only
```

The generated `runtime-manifest.json` records the exact binary digest, PE
machine, CMake options, and toolchain and is included in the MSI. The generated
directory is intentionally ignored by Git.

### Build Mozc

Assuming `bazelisk` is in your `%PATH%`, run the following command to build Mozc
for Windows.

```
bazelisk build package --config release_build
```

#### Install Mozc

After building Mozc, run the following command to install it:

```
python build_tools/open.py bazel-bin/win32/installer/Mozc64.msi
```

#### Uninstall Mozc

To Uninstall Mozc, press <kbd>Win</kbd>+<kbd>R</kbd> to open the Run dialog and
type `ms-settings:appsfeatures-app`, run the following command in the terminal:

```
start ms-settings:appsfeatures-app
```

Then, uninstall `Mozc` from the list of installed applications.

### Cross compilation

By default, `Mozc64.msi` is built for the host CPU architecture. To explicitly
specify the target CPU architecture, specify build options as follows:

#### To build x64 installer

```
python ..\tools\release\prepare_windows_zenz_runtime.py --arch x64
python ..\tools\release\prepare_windows_zenz_runtime.py --arch x64 --verify-only
python build_tools/build_qt.py --release --confirm_license --target_arch=x64
bazelisk build package --config release_build --platforms=//:windows-x86_64
```

#### To build ARM64 installer

```
python ..\tools\release\prepare_windows_zenz_runtime.py --arch arm64
python ..\tools\release\prepare_windows_zenz_runtime.py --arch arm64 --verify-only
python build_tools/build_qt.py --release --confirm_license --target_arch=arm64
bazelisk build package --config release_build --platforms=//:windows-arm64
```

The runtime preparation command discovers `vcvarsall.bat`, imports its
`amd64_arm64` SDK environment, and then uses the checksum-pinned LLVM 22.1.8
and Ninja 1.13.2 installed by `update_deps.py`. It configures llama.cpp with the
pinned `cmake/arm64-windows-llvm.cmake` cross-toolchain; no separate Ninja or
LLVM installation is used.

#### To build a universal installer for both X64 and ARM64

To build an X64 installer that is also compatible with ARM64 machines, run the
following commands:

```
python ..\tools\release\prepare_windows_zenz_runtime.py --arch x64
python build_tools/build_qt.py --release --confirm_license --target_arch=x64
bazelisk build package --config release_build --platforms=//:windows-x86_64 --config win_universal_installer
```

The universal MSI keeps its Mozc server, scorer, and pinned Zenz runtime x64
and adds the ARM64/ARM64EC TIP bridge binaries; those x64 processes run through
Windows-on-ARM emulation. The native ARM64 MSI instead requires and verifies an
ARM64 Zenz runtime. Bazel selects between the generated inputs by target
platform, and the runtime verifier rejects a mismatched PE machine.

Building the ARM64 and universal installers requires the ARM64/ARM64EC Visual
Studio components listed in [Software Requirements](#software-requirements).

## Bazel command examples

### Bazel User Guide

*   [Build programs with Bazel](https://bazel.build/run/build)
*   [Commands and Options](https://bazel.build/docs/user-manual)
*   [Write bazelrc configuration files](https://bazel.build/run/bazelrc)

### Run all tests

```
bazelisk test ... --build_tests_only -c dbg
```

> [!NOTE] `...` means all targets under the current and subdirectories.

### Mozkey Grimodex desktop contract tests

The Windows workflow also runs the portable Protocol v1 fixtures, the secure
Windows reader/registrar tests, TSF context and edit-session tests, and renderer
callback provenance tests. See
[Grimodex Protocol v1 on Windows and macOS](grimodex_desktop_integration.md)
for the focused command and the security/lifecycle contract.

--------------------------------------------------------------------------------

## Build with GitHub Actions

GitHub Actions are already set up in
[windows.yaml](../.github/workflows/windows.yaml). Pull requests and ordinary
pushes run the Windows test gate only; they do not build or upload an MSI.
Installer builds are called by the release workflow from a validated release
tag.

1.  Fork https://github.com/kazormia296/mozkey-ibg to your GitHub repository.
2.  Update `src/version.bzl`, merge the tested change into `main`, and create an
    annotated `vX.Y.Z` tag with the same version.
3.  Push the tag and wait for the **Mozkey Release** workflow to succeed.
4.  Review the generated draft prerelease and its checksums.
5.  Download the architecture-specific `MozkeyIbG_vX.Y.Z_*.msi` after publishing
    the release.

See [Releasing Mozkey](releasing.md) for the exact version, ancestry, release
notes, and rerun contract.

You can also find Mozkey installers for Windows in the Mozkey repository.
Please keep in mind that Mozc is not an officially supported Google product,
even if downloaded from https://github.com/kazormia296/mozkey-ibg/.

1.  Sign in GitHub.
2.  Open the repository's
    [Releases](https://github.com/kazormia296/mozkey-ibg/releases) page.
3.  Download the `x64`, `universal`, or `arm64` MSI that matches the target
    machine.

--------------------------------------------------------------------------------

## Build with GYP (deprecated):

⚠️ The GYP build is deprecated and no longer supported.

Please check the previous version for more information.
https://github.com/google/mozc/blob/3.33.6089/docs/build_mozc_in_windows.md#build-with-gyp-maintenance-mode
