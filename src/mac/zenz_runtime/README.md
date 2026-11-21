# macOS Zenz runtime (superseded)

> **Superseded by the sync to upstream Mozkey v1.0.0.** `//mac:package` now
> packages upstream Mozkey's Universal runtime from
> `src/mac/installer/zenz_runtime` (`Resources/ZenzRuntime`, built by
> `build_universal_runtime.zsh`). Nothing consumes this package or the
> `generated/` tree any more; it is kept only until it is removed from the
> repository. `tools/release/prepare_macos_zenz_runtime.py` remains the shared
> llama.cpp pin module for the Linux and Windows runtime tooling.

The remainder of this file describes the previous, fork-specific arm64 runtime.

`tools/release/prepare_macos_zenz_runtime.py` creates the ignored `generated/`
tree consumed by the macOS package target. The preparation gate:

- verifies the official llama.cpp `v1.0.0` source archive SHA-256;
- builds a static CPU/Accelerate-only `arm64` (macOS 12.0) `llama-server`;
- compiles and stages a matching `arm64` `mozc_zenz_scorer` with the same
  deployment target;
- disables Metal, CURL, RPC, dynamic backends, multimodal video, tests,
  examples, the unified app, and the server Web UI; the tools subtree is
  configured because llama.cpp defines `llama-server` there, but the preparation
  tool builds and stages only the explicit `llama-server` target;
- creates and verifies the repository-pinned normalized Zenz GGUF; and
- stages a machine-readable runtime manifest.

The generated tree is deliberately absent from source control. Consequently,
`//mac:package` cannot silently produce a Zenz-enabled package without the
verified runtime inputs.
