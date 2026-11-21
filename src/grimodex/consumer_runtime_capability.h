// Copyright 2026 The Mozkey Authors

#ifndef MOZC_GRIMODEX_CONSUMER_RUNTIME_CAPABILITY_H_
#define MOZC_GRIMODEX_CONSUMER_RUNTIME_CAPABILITY_H_

#include <functional>

#include "absl/strings/string_view.h"

namespace mozc::grimodex {

// Returns true only when every installed file needed by the Windows Zenz v3
// execution path is present as a nonempty regular file.  Paths passed to the
// probe are relative to the Mozkey server directory.
bool HasCompleteWindowsZenzRuntime(
    const std::function<bool(absl::string_view)> &file_probe);

// Returns true only when every file consumed by the packaged macOS scorer is
// present under MozkeyIbG.app/Contents/Resources/ZenzRuntime.
bool HasCompleteMacosZenzRuntime(
    const std::function<bool(absl::string_view)> &file_probe);

}  // namespace mozc::grimodex

#endif  // MOZC_GRIMODEX_CONSUMER_RUNTIME_CAPABILITY_H_
