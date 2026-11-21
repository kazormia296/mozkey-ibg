# Mozkey IbG macOS installer: isolated regression tests

The OSS macOS package has two installation gates. A normal guarded build is
**not installable**; an explicitly opted-in build uses the release preinstall
script. Every installation path and package ID is Mozkey IbG's own (see
`docs/mozkey_ibg_product_identity.md`); the scripts never inspect, gate on or
modify an upstream Mozkey (`Mozc.app`, `org.mozc.*`) installed side by side.
`//mac:tweak_preflight` and `//mac:tweak_postflight` fail the build if an
upstream identifier appears in a generated script.

## Regression test

From the repository root, run:

```sh
python3 src/mac/preflight_mozkey_release_test.py
```

Expected: `MOZKEY_PREFLIGHT_FIXTURES=31/31` and exit status 0.

The test reads `src/version.bzl` and
`src/mac/preflight_mozkey_release.sh`. Each case makes its **own temporary**
filesystem tree and runs **a transformed copy** of the preinstall script with
fake `pkgutil` and `PlistBuddy` executables. It does not require root or Bazel.
It never runs the unmodified preinstall on the host. The test aborts if
expected hard-coded paths or tool replacements cannot be recognized.

The 31 cases cover clean install, valid orphan receipts, version/downgrade
refusal, the legacy format left by Mozkey IbG packages up to v0.10.4 (receipt
version `0`, bundle version `3.33.x`), wrong receipt identity/volume/location,
residual app directories and LaunchAgents, symlinks, unreadable receipts,
installed-app consistency and non-root installation targets.

To save machine-readable results without modifying the repository:

```sh
python3 src/mac/preflight_mozkey_release_test.py \
  --report /tmp/mozkey_preinstall_fixture_results.json
```

**Coverage boundary:** these are synthetic preinstall decision tests, not a
replacement for macOS `pkgbuild`/`productbuild`, codesign verification, UI/TIS
checks or an actual PackageKit reinstall. They do not test the separate
postinstall legacy renderer cleanup.

## Release build safety

The Bazel rule in `src/mac/BUILD.bazel` chooses
`src/mac/preflight_mozkey_stage1.sh` for ordinary OSS builds. This blocks
installation. Only an explicit `--config=oss_macos` plus
`--define=MOZKEY_INSTALLABLE=1` selects
`src/mac/preflight_mozkey_release.sh`. Always inspect the **packaged**
preinstall script and package receipt version before distributing a new PKG.

The Mozkey IbG identifiers are:

- App: `/Library/Input Methods/MozkeyIbG.app`
- App bundle ID: `io.github.kazormia296.mozkey-ibg.inputmethod.Japanese`
- Package receipt: `io.github.kazormia296.mozkey-ibg.pkg.JapaneseInput`
- Uninstaller: `/Applications/MozkeyIbG/UninstallMozkeyIbG.app`
- Installer (receipt and `CFBundleVersion`) version: `100 + major` of the
  release version, e.g. `100.10.4` for v0.10.4.

The upstream Mozkey preinstall was verified on a physical Mac by its
maintainer. The Mozkey IbG conversion of these scripts, including the legacy
v0.10.x upgrade path, is covered by the fixtures above only and still needs a
physical install, upgrade and uninstall check before a release.
