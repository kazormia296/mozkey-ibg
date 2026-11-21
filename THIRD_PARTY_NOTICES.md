# Third-party notices

This project includes third-party software, model files, and generated
dictionary data.

## Bundled runtime and model notices

Platform-specific notices for bundled Zenz runtime and model files are maintained at:

- Windows: `src/win32/installer/zenz_runtime/licenses/THIRD_PARTY_NOTICES.md`
- macOS: `src/mac/installer/zenz_runtime/licenses/THIRD_PARTY_NOTICES.md`

These notices cover the source Zenz GGUF model, Mozkey's Windows and Linux
metadata-normalized derivative and modification notice, the patched llama.cpp
runtime and unmodified model shipped on macOS, llama.cpp / ggml runtime
components and their bundled HTTP/JSON dependencies, and the local inference
privacy note. The Windows notice is also the one shipped with the Linux Zenz
runtime.

## Fcitx5 addon integration

The Fcitx5 integration under `src/unix/fcitx5/` is based on
`fcitx/mozc@102eb176a3153e53bcc7f01fb5e803f9568985a0` and includes local Mozkey
and Grimodex changes. The Fcitx-derived engine/factory/i18n implementation and
translation catalogs are distributed under GNU LGPL version 2.1 or any later
version. Fcitx adapter files that carry the explicit Google/Weng Xuetian
BSD-3-Clause header (including the key translator/handler, response parser,
state, surrounding-text utility, and client pool) retain that file-level
license. Independently authored Mozkey/Grimodex files retain their own
file-level BSD-3-Clause or Apache-2.0 notices.

* Source: [https://github.com/fcitx/mozc](https://github.com/fcitx/mozc)
* Corresponding modified source and build scripts: this repository and the
  exact revision accompanying the distributed binary artifact
* LGPL text: `LICENSES/LGPL-2.1-or-later.txt`
* Google/Weng BSD notice:
  `LICENSES/Fcitx5-Mozc-BSD-3-Clause.txt`

## Generated dictionary data

Release-bundled dictionary notices, source revisions and upstream legal
attributions are maintained in
[DICTIONARY_NOTICES.md](src/data/installer/DICTIONARY_NOTICES.md).
The bundled notice also includes `SudachiDict-LEGAL.txt`.

This project can build a broad `daily` dictionary for pinned local evaluation
and a narrower `release-approved-only` profile for public artifacts. The
public profile's exact revisions, payload SHA-256 values, license metadata,
and exclusion policy are documented in
`tools/dictionary/daily_sources.lock.json` and
`tools/dictionary/RELEASE_THIRD_PARTY_NOTICES.md`. The Windows PowerShell
`daily` importers inherited from upstream Mozkey pin their inputs in
`tools/dictionary/locks/daily_sources.json`. All of these profiles are
generated dictionary data derived from third-party sources.

Generated dictionary artifacts are not committed to this repository by default.
Before redistributing a built package that includes generated dictionaries,
check the upstream source licenses, attribution requirements, and distribution
notes.

### Mozc

* Source: [https://github.com/google/mozc](https://github.com/google/mozc)
* Used for: base dictionary, id.def, dictionary build system
* License note: Mozc code is BSD-3-Clause. Dictionary data under
  `src/data/dictionary_oss/` is mixed; see
  `src/data/dictionary_oss/README.txt` in the upstream Mozc source tree.

### merge-ut-dictionaries

* Source: [https://github.com/utuhiro78/merge-ut-dictionaries](https://github.com/utuhiro78/merge-ut-dictionaries)
* Used for: merge-ut daily dictionary profile
* License note: the pinned upstream README describes the generated
  `mozcdic-ut.txt` as `Mixed`, while its LICENSE file says `Combined`.
  These descriptions are not a single license grant for all entries.
* Data/license note: upstream lists source-specific license notes including
  Japanese Wikipedia-derived data, Mozc dictionary data, id.def, and source code.

### dic-nico-intersection-pixiv

* Default source of the upstream-inherited Windows `daily` importer:
  [Niccori250k fork](https://github.com/Niccori250k/dic-nico-intersection-pixiv),
  at the pinned revision recorded in `tools/dictionary/locks/daily_sources.json`.
* Original project: [ncaq/dic-nico-intersection-pixiv](https://github.com/ncaq/dic-nico-intersection-pixiv),
  pinned for the `local-evaluation` profile in
  `tools/dictionary/daily_sources.lock.json`.
* Used for: pinned local-evaluation nico/pixiv delta dictionary only
* Provenance: the importer verifies the pinned revision, file size, and SHA-256,
  and records the source details in a generated manifest.
* License / generated output: both project READMEs state that the source code is
  MIT-licensed and that the authors do not claim copyright over the generated
  dictionary output.
* Data note: upstream describes the dictionary as using data from Niconico
  Encyclopedia and Pixiv Encyclopedia. Mozkey IbG keeps it excluded from public
  CI artifacts and installed products (`release_approved: false` in
  `tools/dictionary/daily_sources.lock.json`) because the redistribution
  conditions of the underlying encyclopedia data are not sufficiently explicit.

### mozcdic-ut-personal-names

* Source: [https://github.com/utuhiro78/mozcdic-ut-personal-names](https://github.com/utuhiro78/mozcdic-ut-personal-names)
* Used for: personal names dictionary
* License: Apache License, Version 2.0

### Mozkey syntax guard dictionary

* Source: this repository
* Used for: small grammar-boundary guard dictionary
* Generated by: `tools/dictionary/generate_syntax_guard_dictionary.py`
