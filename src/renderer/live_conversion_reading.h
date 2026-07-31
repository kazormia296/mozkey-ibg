// Copyright 2026 The Mozkey Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MOZC_RENDERER_LIVE_CONVERSION_READING_H_
#define MOZC_RENDERER_LIVE_CONVERSION_READING_H_

#include <string>

#include "protocol/commands.pb.h"

namespace mozc::renderer {

// Returns the typed reading represented by a live-conversion output. Each
// preedit segment's key is preferred, with its visible value as a fallback.
// Returns an empty string for non-live-conversion output or malformed UTF-8.
std::string BuildLiveConversionReading(const commands::Output& output);

}  // namespace mozc::renderer

#endif  // MOZC_RENDERER_LIVE_CONVERSION_READING_H_
