# Third-party notices

This product includes third-party software and model files.

## Zenz v3.2 small GGUF

- Repository: Miwa-Keita/zenz-v3.2-small-gguf
- Source: Hugging Face
- Original model file: ggml-model-Q5_K_M.gguf
- Included file: models/zenz-v3.2-small-Q5_K_M.gguf
- License: Apache License 2.0
- Source repository URL: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf
- Source file URL: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/blob/main/ggml-model-Q5_K_M.gguf
- Source commit observed for the model upload: c67e03e
- Notes: Distributed without modification except for file placement, file naming, and packaging into the macOS PKG installer.

## llama.cpp / ggml runtime

- Project: ggml-org/llama.cpp
- Source repository URL: https://github.com/ggml-org/llama.cpp
- Source tag: v0.7.0
- Source commit: 620d2225cc436ab0b001e3200e590e3b286ef9ab
- Included executable: llama-server (macOS Universal, arm64 and x86_64)
- License: MIT License
- Packaged license file: `llama.cpp-MIT.txt`
- Modification notice: built with the tracked patches
  `patches/llama-vocab-gpt2-small-japanese-char.patch` (classifies the model's
  `gpt2-small-japanese-char` pre-tokenizer as the GPT-2 vocabulary pre-type) and
  `patches/llama-server-mozkey-dual-liveness.patch` (the worker exits when the
  scorer or its guard process goes away). The exact source and patch identities
  are recorded in the packaged `BUILD-CONTRACT.txt`.
- Notes: Distributed as local inference runtime components used by the bundled Zenz GGUF model.

## cpp-httplib

- Project: yhirose/cpp-httplib
- Source repository URL: https://github.com/yhirose/cpp-httplib
- License: MIT License
- Packaged license file: `cpp-httplib-MIT.txt`
- Notes: Vendored by llama.cpp and statically linked into `llama-server` for its local HTTP endpoint.

## JSON for Modern C++ (nlohmann/json)

- Project: nlohmann/json
- Source repository URL: https://github.com/nlohmann/json
- License: MIT License
- Packaged license file: `nlohmann-json-MIT.txt`
- Notes: Vendored by llama.cpp and statically linked into `llama-server` for JSON request and response handling.

## Privacy note

Zenz live correction is designed to run locally. The bundled runtime is started as a local process and the HTTP inference endpoint is expected to bind to 127.0.0.1 only. User input is not intentionally sent to external servers by this feature.

Zenz feedback learning can be disabled in the settings UI. Left context is sanitized before being included in prompts; sensitive-looking context such as URLs, email addresses, file paths, tokens, and long numeric identifiers is rejected before prompt construction.