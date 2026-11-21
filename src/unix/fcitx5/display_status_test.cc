// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/display_status.h"

#include <string>

#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "testing/gunit.h"

namespace fcitx {
namespace {

using mozc::commands::CandidateWindow;
using mozc::commands::Output;
using mozc::config::Config;

void AddSegment(const std::string& value, const std::string& key,
                Output* output) {
  auto* segment = output->mutable_preedit()->add_segment();
  segment->set_annotation(mozc::commands::Preedit::Segment::UNDERLINE);
  segment->set_value(value);
  segment->set_value_length(1);
  if (!key.empty()) {
    segment->set_key(key);
  }
  output->mutable_preedit()->set_cursor(1);
}

Output ComposingOutput() {
  Output output;
  output.set_consumed(true);
  AddSegment("かんじ", "", &output);
  return output;
}

Output LiveConversionOutput() {
  Output output;
  output.set_consumed(true);
  output.set_live_conversion(true);
  AddSegment("漢字", "かんじ", &output);
  return output;
}

void AddCandidate(const std::string& value, CandidateWindow* window) {
  auto* candidate = window->add_candidate();
  candidate->set_index(window->candidate_size() - 1);
  candidate->set_value(value);
  candidate->set_id(window->candidate_size() - 1);
  window->set_size(window->candidate_size());
  window->set_position(0);
}

CandidateWindow PassiveSuggestion() {
  CandidateWindow window;
  window.set_category(mozc::commands::SUGGESTION);
  AddCandidate("漢字", &window);
  AddCandidate("感じ", &window);
  return window;
}

// ---------------------------------------------------------------------------
// Conversion status

TEST(DisplayStatusTest, StatusResolution) {
  EXPECT_EQ(ResolveConversionDisplayStatus(Output()),
            ConversionDisplayStatus::kHidden);
  EXPECT_EQ(ResolveConversionDisplayStatus(ComposingOutput()),
            ConversionDisplayStatus::kReading);
  EXPECT_EQ(ResolveConversionDisplayStatus(LiveConversionOutput()),
            ConversionDisplayStatus::kMozc);

  // Flags without a preedit attribute nothing.
  Output flags_only;
  flags_only.set_live_conversion(true);
  flags_only.set_zenz_live_correction_applied(true);
  EXPECT_EQ(ResolveConversionDisplayStatus(flags_only),
            ConversionDisplayStatus::kHidden);
}

TEST(DisplayStatusTest, MozcLabel) {
  EXPECT_EQ(ConversionStatusLabel(LiveConversionOutput(),
                                  Config::CONVERSION_STATUS_FULL_LABEL),
            "Mozc");
  EXPECT_EQ(ConversionStatusLabel(LiveConversionOutput(),
                                  Config::CONVERSION_STATUS_SYMBOL),
            "M");
}

TEST(DisplayStatusTest, ZenzPendingWinsOverLiveConversion) {
  Output output = LiveConversionOutput();
  output.set_zenz_live_correction_pending(true);
  EXPECT_EQ(ResolveConversionDisplayStatus(output),
            ConversionDisplayStatus::kZenzPending);
  EXPECT_EQ(
      ConversionStatusLabel(output, Config::CONVERSION_STATUS_FULL_LABEL),
      "Zenz\xE2\x80\xA6");
  EXPECT_EQ(ConversionStatusLabel(output, Config::CONVERSION_STATUS_SYMBOL),
            "\xE2\x80\xA6");

  // A new correction running on top of an applied one is still "pending".
  output.set_zenz_live_correction_applied(true);
  EXPECT_EQ(ResolveConversionDisplayStatus(output),
            ConversionDisplayStatus::kZenzPending);
}

TEST(DisplayStatusTest, ZenzAppliedLabel) {
  Output output = LiveConversionOutput();
  output.set_zenz_live_correction_applied(true);
  EXPECT_EQ(ResolveConversionDisplayStatus(output),
            ConversionDisplayStatus::kZenz);
  EXPECT_EQ(
      ConversionStatusLabel(output, Config::CONVERSION_STATUS_FULL_LABEL),
      "Zenz");
  EXPECT_EQ(ConversionStatusLabel(output, Config::CONVERSION_STATUS_SYMBOL),
            "Z");
}

TEST(DisplayStatusTest, ReadingPhaseHasNoLabel) {
  // Plain composition, and the phase before live conversion starts.
  EXPECT_EQ(ConversionStatusLabel(ComposingOutput(),
                                  Config::CONVERSION_STATUS_FULL_LABEL),
            "");
  EXPECT_EQ(ConversionStatusLabel(ComposingOutput(),
                                  Config::CONVERSION_STATUS_SYMBOL),
            "");

  Output pre_live = ComposingOutput();
  pre_live.set_pre_live_conversion_reading(true);
  EXPECT_EQ(
      ConversionStatusLabel(pre_live, Config::CONVERSION_STATUS_FULL_LABEL),
      "");

  // Nothing composed at all.
  EXPECT_EQ(ConversionStatusLabel(Output(),
                                  Config::CONVERSION_STATUS_FULL_LABEL),
            "");
}

TEST(DisplayStatusTest, ColorOnlyStyleHasNoText) {
  Output output = LiveConversionOutput();
  output.set_zenz_live_correction_applied(true);
  EXPECT_EQ(
      ConversionStatusLabel(output, Config::CONVERSION_STATUS_COLOR_ONLY), "");
}

TEST(DisplayStatusTest,
     FocusedCandidateListHidesLabelButPassiveSuggestionKeepsIt) {
  Output passive = LiveConversionOutput();
  *passive.mutable_candidate_window() = PassiveSuggestion();
  EXPECT_EQ(
      ConversionStatusLabel(passive, Config::CONVERSION_STATUS_FULL_LABEL),
      "Mozc");

  Output focused_suggestion = passive;
  focused_suggestion.mutable_candidate_window()->set_focused_index(0);
  EXPECT_EQ(ConversionStatusLabel(focused_suggestion,
                                  Config::CONVERSION_STATUS_FULL_LABEL),
            "");

  Output conversion = passive;
  conversion.mutable_candidate_window()->set_category(
      mozc::commands::CONVERSION);
  EXPECT_EQ(
      ConversionStatusLabel(conversion, Config::CONVERSION_STATUS_FULL_LABEL),
      "");

  Output prediction = passive;
  prediction.mutable_candidate_window()->set_category(
      mozc::commands::PREDICTION);
  prediction.mutable_candidate_window()->set_focused_index(1);
  EXPECT_EQ(
      ConversionStatusLabel(prediction, Config::CONVERSION_STATUS_FULL_LABEL),
      "");
}

// ---------------------------------------------------------------------------
// Passive suggestion

TEST(DisplayStatusTest, PassiveSuggestionWindow) {
  EXPECT_TRUE(IsPassiveSuggestionWindow(PassiveSuggestion()));

  // Being navigated.
  CandidateWindow focused = PassiveSuggestion();
  focused.set_focused_index(0);
  EXPECT_FALSE(IsPassiveSuggestionWindow(focused));

  // Not a suggestion.  The category defaults to CONVERSION.
  CandidateWindow conversion = PassiveSuggestion();
  conversion.set_category(mozc::commands::CONVERSION);
  EXPECT_FALSE(IsPassiveSuggestionWindow(conversion));
  CandidateWindow no_category = PassiveSuggestion();
  no_category.clear_category();
  EXPECT_FALSE(IsPassiveSuggestionWindow(no_category));
  CandidateWindow prediction = PassiveSuggestion();
  prediction.set_category(mozc::commands::PREDICTION);
  EXPECT_FALSE(IsPassiveSuggestionWindow(prediction));

  // Nothing to click.
  CandidateWindow empty;
  empty.set_category(mozc::commands::SUGGESTION);
  EXPECT_FALSE(IsPassiveSuggestionWindow(empty));
}

// ---------------------------------------------------------------------------
// Reading

TEST(DisplayStatusTest, LiveConversionReadingUsesSegmentKeys) {
  EXPECT_EQ(BuildDisplayReading(LiveConversionOutput()), "かんじ");
}

TEST(DisplayStatusTest, PreLiveConversionReadingIsExposed) {
  Output output = ComposingOutput();
  output.set_pre_live_conversion_reading(true);
  EXPECT_EQ(BuildDisplayReading(output), "かんじ");

  // Several segments, with and without a key.
  AddSegment("n", "ん", &output);
  EXPECT_EQ(BuildDisplayReading(output), "かんじん");
}

TEST(DisplayStatusTest, NoFlagsReturnsEmpty) {
  EXPECT_EQ(BuildDisplayReading(ComposingOutput()), "");
  EXPECT_EQ(BuildDisplayReading(Output()), "");

  // The flag alone, without a preedit, exposes nothing.
  Output flag_only;
  flag_only.set_pre_live_conversion_reading(true);
  EXPECT_EQ(BuildDisplayReading(flag_only), "");
}

TEST(DisplayStatusTest, MalformedReadingIsNotShown) {
  Output output;
  output.set_consumed(true);
  output.set_pre_live_conversion_reading(true);
  AddSegment("\xFF\xFE", "", &output);
  EXPECT_EQ(BuildDisplayReading(output), "");
}

// ---------------------------------------------------------------------------
// Aux line

TEST(DisplayStatusTest, AuxTextComposition) {
  EXPECT_EQ(BuildAuxText("", "", ""), "");
  EXPECT_EQ(BuildAuxText("かんじ", "", ""), "かんじ");
  EXPECT_EQ(BuildAuxText("", "Mozc", ""), "Mozc");
  EXPECT_EQ(BuildAuxText("", "", "1/5"), "[1/5]");
  EXPECT_EQ(BuildAuxText("かんじ", "Zenz", ""), "かんじ Zenz");
  EXPECT_EQ(BuildAuxText("", "Zenz", "1/5"), "Zenz [1/5]");
  EXPECT_EQ(BuildAuxText("かんじ", "Zenz", "1/5"), "かんじ Zenz [1/5]");
}

TEST(DisplayStatusTest, AuxTextWithoutStatusIsUnchanged) {
  // The format used before the status label existed.
  EXPECT_EQ(BuildAuxText("かんじ", "", "1/5"), "かんじ [1/5]");
  EXPECT_EQ(BuildAuxText("", "", "No result"), "[No result]");
}

TEST(DisplayStatusTest, StatusLabelNeverOpensAPopupByItself) {
  // Client preedit, reading turned off, no footer: nothing is shown in the
  // panel while typing, and the label must not change that.
  EXPECT_FALSE(StatusLabelHasHost(/*has_reading=*/false, /*has_footer=*/false,
                                  /*panel_shows_preedit=*/false));

  // Next to the reading, next to a candidate footer, or on the line of a
  // preedit the panel draws itself.
  EXPECT_TRUE(StatusLabelHasHost(true, false, false));
  EXPECT_TRUE(StatusLabelHasHost(false, true, false));
  EXPECT_TRUE(StatusLabelHasHost(false, false, true));
  EXPECT_TRUE(StatusLabelHasHost(true, true, true));
}

}  // namespace
}  // namespace fcitx
