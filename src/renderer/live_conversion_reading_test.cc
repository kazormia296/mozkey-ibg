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

#include "protocol/commands.pb.h"
#include "testing/gunit.h"

namespace mozc::renderer {
namespace {

commands::Preedit::Segment* AddSegment(commands::Output* output,
                                       const std::string& value,
                                       const std::string& key) {
  commands::Preedit* preedit = output->mutable_preedit();
  preedit->set_cursor(0);
  commands::Preedit::Segment* segment = preedit->add_segment();
  segment->set_annotation(commands::Preedit::Segment::NONE);
  segment->set_value(value);
  segment->set_value_length(1);
  if (!key.empty()) {
    segment->set_key(key);
  }
  return segment;
}

TEST(LiveConversionReadingTest, IgnoresNonLiveConversionOutput) {
  commands::Output output;
  AddSegment(&output, "入力", "にゅうりょく");

  EXPECT_TRUE(BuildLiveConversionReading(output).empty());
}

TEST(LiveConversionReadingTest, RequiresPreedit) {
  commands::Output output;
  output.set_live_conversion(true);

  EXPECT_TRUE(BuildLiveConversionReading(output).empty());
}

TEST(LiveConversionReadingTest, PrefersKeysAndFallsBackToValues) {
  commands::Output output;
  output.set_live_conversion(true);
  AddSegment(&output, "入力", "にゅうりょく");
  AddSegment(&output, "ちゅう", "");

  EXPECT_EQ(BuildLiveConversionReading(output), "にゅうりょくちゅう");
}

TEST(LiveConversionReadingTest, KeepsReadingWhenValueIsIdentical) {
  commands::Output output;
  output.set_live_conversion(true);
  AddSegment(&output, "ひらがな", "ひらがな");

  EXPECT_EQ(BuildLiveConversionReading(output), "ひらがな");
}

TEST(LiveConversionReadingTest, RejectsMalformedUtf8) {
  commands::Output output;
  output.set_live_conversion(true);
  AddSegment(&output, "入力", std::string("\xC2", 1));

  EXPECT_TRUE(BuildLiveConversionReading(output).empty());
}

}  // namespace
}  // namespace mozc::renderer
