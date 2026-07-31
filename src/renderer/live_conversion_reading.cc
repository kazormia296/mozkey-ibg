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

#include "renderer/live_conversion_reading.h"

#include <string>

#include "base/strings/unicode.h"
#include "protocol/commands.pb.h"

namespace mozc::renderer {

std::string BuildLiveConversionReading(const commands::Output& output) {
  if (!output.live_conversion() || !output.has_preedit()) {
    return {};
  }

  std::string reading;
  const commands::Preedit& preedit = output.preedit();
  for (int i = 0; i < preedit.segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    const std::string& part =
        segment.has_key() && !segment.key().empty() ? segment.key()
                                                   : segment.value();
    if (!strings::IsValidUtf8(part)) {
      return {};
    }
    reading.append(part);
  }
  return reading;
}

}  // namespace mozc::renderer
