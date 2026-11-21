// Copyright 2026 The Mozkey Authors

#include "grimodex/consumer_runtime_capability.h"

#include <set>
#include <string>

#include "absl/strings/string_view.h"
#include "testing/gunit.h"

namespace mozc::grimodex {
namespace {

TEST(ConsumerRuntimeCapabilityTest, RequiresTheCompleteWindowsRuntime) {
  std::set<std::string> missing;
  const auto probe = [&missing](absl::string_view path) {
    return !missing.contains(std::string(path));
  };

  EXPECT_TRUE(HasCompleteWindowsZenzRuntime(probe));
  for (const char *path : {
           "mozc_zenz_scorer.exe",
           "llama-server.exe",
           "ggml.dll",
           "ggml-base.dll",
           "ggml-cpu.dll",
           "llama.dll",
           "models/zenz-v3.2-small-Q5_K_M.gguf",
       }) {
    missing = {std::string(path)};
    EXPECT_FALSE(HasCompleteWindowsZenzRuntime(probe)) << path;
  }
}

TEST(ConsumerRuntimeCapabilityTest, RequiresTheCompleteMacosRuntime) {
  std::set<std::string> missing;
  const auto probe = [&missing](absl::string_view path) {
    return !missing.contains(std::string(path));
  };

  EXPECT_TRUE(HasCompleteMacosZenzRuntime(probe));
  for (const char *path : {
           "mozc_zenz_scorer",
           "mozc_zenz_guard",
           "llama-server",
           "models/zenz-v3.2-small-Q5_K_M.gguf",
           "BUILD-CONTRACT.txt",
       }) {
    missing = {std::string(path)};
    EXPECT_FALSE(HasCompleteMacosZenzRuntime(probe)) << path;
  }
}

}  // namespace
}  // namespace mozc::grimodex
