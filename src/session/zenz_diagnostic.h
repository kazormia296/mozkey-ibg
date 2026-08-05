// Copyright 2026 Mozkey IbG contributors.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Mozkey IbG nor the names of its contributors
// may be used to endorse or promote products derived from this software
// without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef MOZC_SESSION_ZENZ_DIAGNOSTIC_H_
#define MOZC_SESSION_ZENZ_DIAGNOSTIC_H_

#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"

namespace mozc {
namespace session {

// Small JSON object builder used by the opt-in Zenz diagnostic logger.  Binary
// values must be added with AddBase64 so a capture can be replayed byte for
// byte without passing through a platform code page or Unicode normalizer.
class ZenzDiagnosticJson final {
 public:
  ZenzDiagnosticJson() = default;

  void AddString(absl::string_view name, absl::string_view value);
  void AddUint64(absl::string_view name, uint64_t value);
  void AddBool(absl::string_view name, bool value);
  // Adds an already-built JSON object.  This is used only for fixed internal
  // diagnostic metadata such as runtime_args.
  void AddObject(absl::string_view name, absl::string_view object);
  void AddBase64(absl::string_view name, absl::string_view bytes);
  void AddCodepoints(absl::string_view name, absl::string_view utf8);

  // Returns one JSON object without a trailing newline.
  std::string Build() const;

 private:
  void AddRawValue(absl::string_view name, std::string value);

  std::string fields_;
};

// Writes one JSON object per line when MOZC_ZENZ_DIAGNOSTIC_JSONL names a
// file.  On Windows, the persistent per-user environment value takes priority
// over the process environment because a long-lived launcher can retain a
// stale inherited value after the user changes the setting.
// The setting is intentionally path-only: an unset value means that no prompt,
// context, model output, or response is persisted.  Logging failures are
// ignored and never affect IME behavior.
class ZenzDiagnosticCapture final {
 public:
  ZenzDiagnosticCapture() = delete;

  static bool IsEnabled();
  static void Write(const ZenzDiagnosticJson& event);

  // Returns a lowercase hexadecimal SHA-256 for a regular file, or an empty
  // string when the path cannot be read.  The result is cached for the life
  // of the process because a model/runtime file is immutable during a scorer
  // session.
  static std::string Sha256File(absl::string_view path);
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ZENZ_DIAGNOSTIC_H_
