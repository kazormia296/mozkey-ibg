// Copyright 2010-2021, Google Inc.
// All rights reserved.
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
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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

#include "session/zenz_output_validator.h"

#include "testing/gunit.h"

namespace mozc::session {
namespace {

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleFullwidthBrackets) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "これは（てすと）です", "これは（テスト）です",
                "これは(テスト)だと思います"),
            "これは（テスト）だと思います");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleAsciiBrackets) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "これは(test)です", "これは(test)です",
                "これは（test）です"),
            "これは(test)です");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleMixedBrackets) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "（A）と(B)", "（A）と(B)", "(A)と(B)"),
            "（A）と(B)");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleQuestionAndBang) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "本当ですか？！", "本当ですか？！", "本当ですか?!"),
            "本当ですか？！");

  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "OK?!", "OK?!", "OK？！"),
            "OK?!");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleKeepsWaveDash) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "ほ〜", "ほ〜", "ほ~"),
            "ほ〜");

  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "ほ~", "ほ~", "ほ〜"),
            "ほ~");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleFullwidthAsciiPunct) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "注：A；B，C．", "注：A；B，C．", "注:A;B,C."),
            "注：A；B，C．");

  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "note:A;B,C.", "note:A;B,C.", "note：A；B，C．"),
            "note:A;B,C.");
}

TEST(ZenzOutputValidatorTest, RejectsReportedPathologicalDoumekiOutput) {
  ZenzValidationInput input;
  input.key = "どうめき";
  input.mozc_value = "百目鬼";
  input.zenz_value = "⼊⼊彪瞰矜矜矜矜矜";
  input.min_key_length = 2;

  const ZenzValidationResult result = ZenzOutputValidator().Validate(input);
  EXPECT_FALSE(result.accept);
  EXPECT_EQ(result.reason, "unexpected_cjk_component");
}

TEST(ZenzOutputValidatorTest, RejectsNewRepeatedIdeographRun) {
  ZenzValidationInput input;
  input.key = "どうめき";
  input.mozc_value = "百目鬼";
  input.zenz_value = "彪瞰矜矜矜矜矜";
  input.min_key_length = 2;

  const ZenzValidationResult result = ZenzOutputValidator().Validate(input);
  EXPECT_FALSE(result.accept);
  EXPECT_EQ(result.reason, "repeated_ideograph");
}

TEST(ZenzOutputValidatorTest, AllowsOrdinaryRareNameCorrection) {
  ZenzValidationInput input;
  input.key = "どうめき";
  input.mozc_value = "道目木";
  input.zenz_value = "百目鬼";
  input.min_key_length = 2;

  const ZenzValidationResult result = ZenzOutputValidator().Validate(input);
  EXPECT_TRUE(result.accept);
}

}  // namespace
}  // namespace mozc::session
