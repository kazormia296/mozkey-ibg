// Copyright 2026 The Mozkey Authors

#include "grimodex/consumer_runtime_capability.h"

#include <array>
#include <functional>

#include "absl/strings/string_view.h"

namespace mozc::grimodex {

bool HasCompleteWindowsZenzRuntime(
    const std::function<bool(absl::string_view)> &file_probe) {
  constexpr std::array<absl::string_view, 7> kRequiredFiles = {
      "mozc_zenz_scorer.exe",
      "llama-server.exe",
      "ggml.dll",
      "ggml-base.dll",
      "ggml-cpu.dll",
      "llama.dll",
      "models/zenz-v3.2-small-Q5_K_M.gguf",
  };
  for (const absl::string_view path : kRequiredFiles) {
    if (!file_probe(path)) {
      return false;
    }
  }
  return true;
}

bool HasCompleteMacosZenzRuntime(
    const std::function<bool(absl::string_view)> &file_probe) {
  // Upstream Mozkey v1.0.0 macOS runtime layout (Resources/ZenzRuntime): the
  // scorer always starts llama-server through mozc_zenz_guard, and the staged
  // runtime carries its source contract next to the worker.
  constexpr std::array<absl::string_view, 5> kRequiredFiles = {
      "mozc_zenz_scorer",
      "mozc_zenz_guard",
      "llama-server",
      "models/zenz-v3.2-small-Q5_K_M.gguf",
      "BUILD-CONTRACT.txt",
  };
  for (const absl::string_view path : kRequiredFiles) {
    if (!file_probe(path)) {
      return false;
    }
  }
  return true;
}

}  // namespace mozc::grimodex
