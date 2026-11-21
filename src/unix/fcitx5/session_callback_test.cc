// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/session_callback.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "protocol/commands.pb.h"
#include "testing/gunit.h"

namespace fcitx {
namespace {

using mozc::commands::KeyEvent;
using mozc::commands::Output;
using mozc::commands::SessionCommand;

Output OutputWithCallback(SessionCommand::CommandType type) {
  Output output;
  output.set_consumed(true);
  output.mutable_callback()->mutable_session_command()->set_type(type);
  return output;
}

Output OutputWithDelayedCallback(SessionCommand::CommandType type,
                                 uint32_t delay_msec) {
  Output output = OutputWithCallback(type);
  output.mutable_callback()->set_delay_millisec(delay_msec);
  return output;
}

void AddSegment(const std::string& value, Output* output) {
  auto* segment = output->mutable_preedit()->add_segment();
  segment->set_annotation(mozc::commands::Preedit::Segment::UNDERLINE);
  segment->set_value(value);
  segment->set_value_length(1);
  output->mutable_preedit()->set_cursor(1);
}

KeyEvent SpaceKey() {
  KeyEvent key;
  key.set_special_key(KeyEvent::SPACE);
  return key;
}

Output HalfWidthSpaceFallback() {
  Output output =
      OutputWithCallback(SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE);
  output.mutable_result()->set_type(mozc::commands::Result::STRING);
  output.mutable_result()->set_value(" ");
  return output;
}

// ---------------------------------------------------------------------------
// Delayed callbacks

TEST(SessionCallbackTest, NoCallbackIsNone) {
  Output output;
  output.set_consumed(true);
  EXPECT_EQ(PlanCallback(output, /*secure_input=*/false).action,
            CallbackAction::kNone);

  // A callback without a command, or a command without a type, is unusable.
  output.mutable_callback()->set_delay_millisec(10);
  EXPECT_EQ(PlanCallback(output, false).action, CallbackAction::kNone);
  output.mutable_callback()->mutable_session_command();
  EXPECT_EQ(PlanCallback(output, false).action, CallbackAction::kNone);
}

TEST(SessionCallbackTest, ResumeCommandSequenceIsEchoedWithGeneration) {
  Output output =
      OutputWithDelayedCallback(SessionCommand::RESUME_COMMAND_SEQUENCE, 150);
  output.mutable_callback()
      ->mutable_session_command()
      ->set_command_sequence_generation(42);

  const CallbackPlan plan = PlanCallback(output, /*secure_input=*/false);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_EQ(plan.delay_msec, 150);
  EXPECT_EQ(plan.command.type(), SessionCommand::RESUME_COMMAND_SEQUENCE);
  ASSERT_TRUE(plan.command.has_command_sequence_generation());
  EXPECT_EQ(plan.command.command_sequence_generation(), 42);
}

TEST(SessionCallbackTest, ResumeWithoutGenerationIsNotGivenOne) {
  // The server rejects a missing generation.  The client must not invent one.
  const CallbackPlan plan = PlanCallback(
      OutputWithDelayedCallback(SessionCommand::RESUME_COMMAND_SEQUENCE, 5),
      /*secure_input=*/false);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_FALSE(plan.command.has_command_sequence_generation());
}

TEST(SessionCallbackTest, DelayedCallbackIsPlannedEvenWhenOutputIsNotConsumed) {
  // A keymap macro keeps the original key's pass-through answer on the
  // Output that carries its continuation.
  Output output =
      OutputWithDelayedCallback(SessionCommand::RESUME_COMMAND_SEQUENCE, 100);
  output.set_consumed(false);
  output.mutable_callback()
      ->mutable_session_command()
      ->set_command_sequence_generation(7);

  const CallbackPlan plan = PlanCallback(output, /*secure_input=*/false);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_EQ(plan.command.command_sequence_generation(), 7);
}

TEST(SessionCallbackTest, LiveConversionCallbackKeepsGenerationAndKey) {
  Output output =
      OutputWithDelayedCallback(SessionCommand::APPLY_LIVE_CONVERSION, 228);
  SessionCommand* command =
      output.mutable_callback()->mutable_session_command();
  command->set_live_conversion_generation(9);
  command->set_live_conversion_key("かんじ");

  const CallbackPlan plan = PlanCallback(output, /*secure_input=*/false);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_EQ(plan.delay_msec, 228);
  EXPECT_EQ(plan.command.SerializeAsString(), command->SerializeAsString());
  EXPECT_EQ(plan.command.live_conversion_generation(), 9);
  EXPECT_EQ(plan.command.live_conversion_key(), "かんじ");
}

TEST(SessionCallbackTest, ZenzPollWithoutDelayIsStillRunFromTheEventLoop) {
  Output output =
      OutputWithCallback(SessionCommand::APPLY_ZENZ_LIVE_CORRECTION);
  output.mutable_callback()
      ->mutable_session_command()
      ->set_live_conversion_generation(3);

  const CallbackPlan plan = PlanCallback(output, /*secure_input=*/false);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_EQ(plan.delay_msec, 0);
  EXPECT_EQ(plan.command.type(), SessionCommand::APPLY_ZENZ_LIVE_CORRECTION);
  EXPECT_EQ(plan.command.live_conversion_generation(), 3);
}

TEST(SessionCallbackTest, UnknownDelayedTypeIsEchoedVerbatim) {
  // A command this client has no special knowledge of: with a delay it is
  // simply sent back, every field included.
  Output output = OutputWithDelayedCallback(SessionCommand::STOP_KEY_TOGGLING,
                                            1000);
  SessionCommand* command =
      output.mutable_callback()->mutable_session_command();
  command->set_id(5);
  command->set_text("payload");
  command->set_renderer_callback_token(77);

  const CallbackPlan plan = PlanCallback(output, /*secure_input=*/false);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_EQ(plan.delay_msec, 1000);
  EXPECT_EQ(plan.command.SerializeAsString(), command->SerializeAsString());
}

TEST(SessionCallbackTest, UnknownImmediateTypeIsDropped) {
  EXPECT_EQ(PlanCallback(OutputWithCallback(SessionCommand::STOP_KEY_TOGGLING),
                         /*secure_input=*/false)
                .action,
            CallbackAction::kDrop);
  EXPECT_EQ(PlanCallback(OutputWithCallback(SessionCommand::SUBMIT),
                         /*secure_input=*/false)
                .action,
            CallbackAction::kDrop);
}

TEST(SessionCallbackTest, ImmediateCallbacksMapToTheirActions) {
  EXPECT_EQ(
      PlanCallback(OutputWithCallback(SessionCommand::UNDO), false).action,
      CallbackAction::kUndo);
  EXPECT_EQ(PlanCallback(OutputWithCallback(SessionCommand::CONVERT_REVERSE),
                         false)
                .action,
            CallbackAction::kConvertReverse);
  EXPECT_EQ(
      PlanCallback(OutputWithCallback(
                       SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE),
                   false)
          .action,
      CallbackAction::kReconvertSelectionOrFallback);
}

TEST(SessionCallbackTest, SecureInputDropsLiveConversionButKeepsResume) {
  for (const SessionCommand::CommandType type :
       {SessionCommand::APPLY_LIVE_CONVERSION,
        SessionCommand::APPLY_ZENZ_LIVE_CORRECTION,
        SessionCommand::CONVERT_REVERSE}) {
    EXPECT_EQ(PlanCallback(OutputWithCallback(type), /*secure_input=*/true)
                  .action,
              CallbackAction::kDrop)
        << type;
    EXPECT_EQ(PlanCallback(OutputWithDelayedCallback(type, 50),
                           /*secure_input=*/true)
                  .action,
              CallbackAction::kDrop)
        << type;
  }

  Output resume =
      OutputWithDelayedCallback(SessionCommand::RESUME_COMMAND_SEQUENCE, 50);
  resume.mutable_callback()
      ->mutable_session_command()
      ->set_command_sequence_generation(11);
  const CallbackPlan plan = PlanCallback(resume, /*secure_input=*/true);
  ASSERT_EQ(plan.action, CallbackAction::kSchedule);
  EXPECT_EQ(plan.command.command_sequence_generation(), 11);

  // Undo and the Space fallback never read application text by themselves.
  EXPECT_EQ(PlanCallback(OutputWithCallback(SessionCommand::UNDO), true).action,
            CallbackAction::kUndo);
  EXPECT_EQ(
      PlanCallback(OutputWithCallback(
                       SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE),
                   true)
          .action,
      CallbackAction::kReconvertSelectionOrFallback);
}

TEST(SessionCallbackTest, SecureInputRuleIsAvailableAtFireTime) {
  // A delayed command armed in a normal field is checked again when its
  // timer fires.
  for (const SessionCommand::CommandType type :
       {SessionCommand::APPLY_LIVE_CONVERSION,
        SessionCommand::APPLY_ZENZ_LIVE_CORRECTION,
        SessionCommand::CONVERT_REVERSE}) {
    EXPECT_TRUE(IsAllowedToRun(type, /*secure_input=*/false)) << type;
    EXPECT_FALSE(IsAllowedToRun(type, /*secure_input=*/true)) << type;
  }
  for (const SessionCommand::CommandType type :
       {SessionCommand::RESUME_COMMAND_SEQUENCE, SessionCommand::UNDO,
        SessionCommand::STOP_KEY_TOGGLING}) {
    EXPECT_TRUE(IsAllowedToRun(type, /*secure_input=*/false)) << type;
    EXPECT_TRUE(IsAllowedToRun(type, /*secure_input=*/true)) << type;
  }
}

TEST(SessionCallbackTest, OnlyResumeIsInvalidatedByKey) {
  EXPECT_TRUE(IsInvalidatedByKey(SessionCommand::RESUME_COMMAND_SEQUENCE));
  EXPECT_FALSE(IsInvalidatedByKey(SessionCommand::APPLY_LIVE_CONVERSION));
  EXPECT_FALSE(IsInvalidatedByKey(SessionCommand::APPLY_ZENZ_LIVE_CORRECTION));
  EXPECT_FALSE(IsInvalidatedByKey(SessionCommand::UNDO));
  EXPECT_FALSE(IsInvalidatedByKey(SessionCommand::STOP_KEY_TOGGLING));
}

TEST(SessionCallbackTest, ResumeCommandCarriesGeneration) {
  const SessionCommand command = MakeResumeCommand(123);
  EXPECT_EQ(command.type(), SessionCommand::RESUME_COMMAND_SEQUENCE);
  ASSERT_TRUE(command.has_command_sequence_generation());
  EXPECT_EQ(command.command_sequence_generation(), 123);
  EXPECT_FALSE(command.has_live_conversion_generation());
  EXPECT_FALSE(command.has_text());
}

// ---------------------------------------------------------------------------
// RECONVERT_SELECTION_OR_INSERT_SPACE

TEST(SessionCallbackTest, PureSpaceWithHalfWidthFallbackPassesThrough) {
  EXPECT_TRUE(
      IsPassThroughSpaceFallback(SpaceKey(), HalfWidthSpaceFallback()));

  // The callback is cleared before the check in the adapter.
  Output fallback = HalfWidthSpaceFallback();
  fallback.clear_callback();
  EXPECT_TRUE(IsPassThroughSpaceFallback(SpaceKey(), fallback));
}

TEST(SessionCallbackTest, FullWidthFallbackIsCommitted) {
  Output fallback = HalfWidthSpaceFallback();
  fallback.mutable_result()->set_value("\xE3\x80\x80");  // U+3000
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), fallback));

  fallback.mutable_result()->set_value("  ");
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), fallback));
}

TEST(SessionCallbackTest, ModifiedOrNonSpaceKeyIsCommitted) {
  const Output fallback = HalfWidthSpaceFallback();

  KeyEvent shift_space = SpaceKey();
  shift_space.add_modifier_keys(KeyEvent::SHIFT);
  EXPECT_FALSE(IsPassThroughSpaceFallback(shift_space, fallback));

  KeyEvent space_with_code = SpaceKey();
  space_with_code.set_key_code(' ');
  EXPECT_FALSE(IsPassThroughSpaceFallback(space_with_code, fallback));

  KeyEvent henkan;
  henkan.set_special_key(KeyEvent::HENKAN);
  EXPECT_FALSE(IsPassThroughSpaceFallback(henkan, fallback));

  KeyEvent letter;
  letter.set_key_code('a');
  EXPECT_FALSE(IsPassThroughSpaceFallback(letter, fallback));

  EXPECT_FALSE(IsPassThroughSpaceFallback(KeyEvent(), fallback));
}

TEST(SessionCallbackTest, ModeHintDoesNotChangeThePureSpaceDecision) {
  KeyEvent key = SpaceKey();
  ApplySessionModeHint(mozc::commands::HIRAGANA, &key);
  EXPECT_TRUE(IsPassThroughSpaceFallback(key, HalfWidthSpaceFallback()));
}

TEST(SessionCallbackTest, FallbackWithPreeditIsNotPassedThrough) {
  Output with_preedit = HalfWidthSpaceFallback();
  AddSegment("a", &with_preedit);
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), with_preedit));

  Output with_candidates = HalfWidthSpaceFallback();
  with_candidates.mutable_candidate_window()->set_size(0);
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), with_candidates));

  Output with_deletion = HalfWidthSpaceFallback();
  with_deletion.mutable_deletion_range()->set_offset(-1);
  with_deletion.mutable_deletion_range()->set_length(1);
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), with_deletion));
}

TEST(SessionCallbackTest, UnconsumedOrResultlessFallbackIsLeftAlone) {
  // An older server echoes the key back itself: nothing to rewrite.
  Output echoed_back = HalfWidthSpaceFallback();
  echoed_back.clear_result();
  echoed_back.set_consumed(false);
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), echoed_back));

  Output consumed_without_result = HalfWidthSpaceFallback();
  consumed_without_result.clear_result();
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), consumed_without_result));

  Output none_result = HalfWidthSpaceFallback();
  none_result.mutable_result()->set_type(mozc::commands::Result::NONE);
  EXPECT_FALSE(IsPassThroughSpaceFallback(SpaceKey(), none_result));
}

TEST(SessionCallbackTest, SmallSingleLineSelectionIsReconvertible) {
  EXPECT_EQ(ClassifySelectionForReconversion("abc", 3),
            SelectionForReconversion::kReconvertible);
  // Backward selection.
  EXPECT_EQ(ClassifySelectionForReconversion("abc", -3),
            SelectionForReconversion::kReconvertible);
  // The length is counted in characters, not bytes.
  EXPECT_EQ(ClassifySelectionForReconversion("漢字", 2),
            SelectionForReconversion::kReconvertible);
  EXPECT_EQ(ClassifySelectionForReconversion("東京 タワー", 6),
            SelectionForReconversion::kReconvertible);

  const std::string longest(kMaxReconvertSelectionChars, 'a');
  EXPECT_EQ(ClassifySelectionForReconversion(
                longest, static_cast<int32_t>(kMaxReconvertSelectionChars)),
            SelectionForReconversion::kReconvertible);
}

TEST(SessionCallbackTest, NoSelectionIsNotClassifiedAsOne) {
  EXPECT_EQ(ClassifySelectionForReconversion("", 0),
            SelectionForReconversion::kNone);
  // Cursor and anchor are equal: the text is not a selection.
  EXPECT_EQ(ClassifySelectionForReconversion("abc", 0),
            SelectionForReconversion::kNone);
  // The toolkit reported positions but no text between them.
  EXPECT_EQ(ClassifySelectionForReconversion("", 3),
            SelectionForReconversion::kNone);
}

TEST(SessionCallbackTest, SelectionLengthMismatchIsNotVerified) {
  // The reported text is shorter than the reported selection: deleting by
  // position would remove text that was never read.
  EXPECT_EQ(ClassifySelectionForReconversion("ab", 3),
            SelectionForReconversion::kNone);
  EXPECT_EQ(ClassifySelectionForReconversion("abcd", 3),
            SelectionForReconversion::kNone);
  EXPECT_EQ(ClassifySelectionForReconversion("漢字", 6),
            SelectionForReconversion::kNone);
  EXPECT_EQ(ClassifySelectionForReconversion("abc", -4),
            SelectionForReconversion::kNone);
  // INT32_MIN cannot be negated; it must simply not match.
  EXPECT_EQ(ClassifySelectionForReconversion(
                "abc", std::numeric_limits<int32_t>::min()),
            SelectionForReconversion::kNone);
}

TEST(SessionCallbackTest, OversizeSelectionIsNeverSent) {
  const size_t too_many = kMaxReconvertSelectionChars + 1;
  EXPECT_EQ(ClassifySelectionForReconversion(std::string(too_many, 'a'),
                                             static_cast<int32_t>(too_many)),
            SelectionForReconversion::kNotReconvertible);
  EXPECT_EQ(ClassifySelectionForReconversion(std::string(too_many, 'a'),
                                             -static_cast<int32_t>(too_many)),
            SelectionForReconversion::kNotReconvertible);

  // "Select all" in a document.
  std::string document;
  for (size_t i = 0; i < 10000; ++i) {
    document += "あ";
  }
  EXPECT_EQ(ClassifySelectionForReconversion(document, 10000),
            SelectionForReconversion::kNotReconvertible);
}

TEST(SessionCallbackTest, MultiLineSelectionIsNeverSent) {
  EXPECT_EQ(ClassifySelectionForReconversion("ab\ncd", 5),
            SelectionForReconversion::kNotReconvertible);
  EXPECT_EQ(ClassifySelectionForReconversion("ab\r\ncd", 6),
            SelectionForReconversion::kNotReconvertible);
  EXPECT_EQ(ClassifySelectionForReconversion("ab\tcd", 5),
            SelectionForReconversion::kNotReconvertible);
  EXPECT_EQ(ClassifySelectionForReconversion("\n", 1),
            SelectionForReconversion::kNotReconvertible);
  // U+2028 LINE SEPARATOR and U+2029 PARAGRAPH SEPARATOR.
  EXPECT_EQ(ClassifySelectionForReconversion("ab\xE2\x80\xA8" "cd", 5),
            SelectionForReconversion::kNotReconvertible);
  EXPECT_EQ(ClassifySelectionForReconversion("ab\xE2\x80\xA9" "cd", -5),
            SelectionForReconversion::kNotReconvertible);
}

TEST(SessionCallbackTest, NoSelectionUsesTheFallback) {
  // Plain Space, half-width fallback: handed back as a key.
  const KeyEvent space = SpaceKey();
  Output pass_through = HalfWidthSpaceFallback();
  ResolveSpaceFallback(SelectionForReconversion::kNone, &space, &pass_through);
  EXPECT_FALSE(pass_through.has_callback());
  EXPECT_FALSE(pass_through.consumed());
  EXPECT_FALSE(pass_through.has_result());

  // Full-width fallback: committed as the server produced it.
  Output full_width = HalfWidthSpaceFallback();
  full_width.mutable_result()->set_value("\xE3\x80\x80");  // U+3000
  ResolveSpaceFallback(SelectionForReconversion::kNone, &space, &full_width);
  EXPECT_FALSE(full_width.has_callback());
  EXPECT_TRUE(full_width.consumed());
  ASSERT_TRUE(full_width.has_result());
  EXPECT_EQ(full_width.result().value(), "\xE3\x80\x80");

  // No key that could be forwarded (a composed string): committed as text.
  Output composed = HalfWidthSpaceFallback();
  ResolveSpaceFallback(SelectionForReconversion::kNone, nullptr, &composed);
  EXPECT_FALSE(composed.has_callback());
  EXPECT_TRUE(composed.consumed());
  ASSERT_TRUE(composed.has_result());
  EXPECT_EQ(composed.result().value(), " ");

  ResolveSpaceFallback(SelectionForReconversion::kNone, &space, nullptr);
}

TEST(SessionCallbackTest, SelectionWithRefusedReconversionConsumesSpace) {
  // The server refused the selected text (or the command failed, or the text
  // was never sent).  The key is consumed and the selection stays untouched:
  // neither the space is committed nor the key forwarded.
  const KeyEvent space = SpaceKey();
  for (const SelectionForReconversion selection :
       {SelectionForReconversion::kReconvertible,
        SelectionForReconversion::kNotReconvertible}) {
    Output half_width = HalfWidthSpaceFallback();
    half_width.set_mode(mozc::commands::HIRAGANA);
    ResolveSpaceFallback(selection, &space, &half_width);
    EXPECT_FALSE(half_width.has_callback());
    EXPECT_TRUE(half_width.consumed());
    EXPECT_FALSE(half_width.has_result());
    EXPECT_FALSE(half_width.has_deletion_range());
    EXPECT_FALSE(HasHostSideEffect(half_width));
    // The rest of the Output (the mode indicator) is still applied.
    EXPECT_EQ(half_width.mode(), mozc::commands::HIRAGANA);

    Output full_width = HalfWidthSpaceFallback();
    full_width.mutable_result()->set_value("\xE3\x80\x80");  // U+3000
    ResolveSpaceFallback(selection, &space, &full_width);
    EXPECT_TRUE(full_width.consumed());
    EXPECT_FALSE(full_width.has_result());

    // Without a key to forward the answer is the same.
    Output composed = HalfWidthSpaceFallback();
    ResolveSpaceFallback(selection, nullptr, &composed);
    EXPECT_TRUE(composed.consumed());
    EXPECT_FALSE(composed.has_result());

    // An older server that echoes the key back must not get the selection
    // replaced by the application either.
    Output echoed_back = HalfWidthSpaceFallback();
    echoed_back.clear_result();
    echoed_back.set_consumed(false);
    ResolveSpaceFallback(selection, &space, &echoed_back);
    EXPECT_TRUE(echoed_back.consumed());
    EXPECT_FALSE(echoed_back.has_callback());

    Output with_deletion = HalfWidthSpaceFallback();
    with_deletion.mutable_deletion_range()->set_offset(-1);
    with_deletion.mutable_deletion_range()->set_length(1);
    ResolveSpaceFallback(selection, &space, &with_deletion);
    EXPECT_FALSE(with_deletion.has_deletion_range());
    EXPECT_FALSE(with_deletion.has_result());
  }
}

TEST(SessionCallbackTest, DoNothingReplyDoesNotStartReconversion) {
  // ConvertReverse answers DoNothing for text it cannot reverse-convert.
  Output do_nothing;
  do_nothing.set_consumed(true);
  EXPECT_FALSE(ReconversionStarted(do_nothing));

  Output empty_preedit;
  empty_preedit.set_consumed(true);
  empty_preedit.mutable_preedit()->set_cursor(0);
  EXPECT_FALSE(ReconversionStarted(empty_preedit));

  Output unconsumed;
  unconsumed.set_consumed(false);
  AddSegment("a", &unconsumed);
  EXPECT_FALSE(ReconversionStarted(unconsumed));

  Output started;
  started.set_consumed(true);
  AddSegment("a", &started);
  EXPECT_TRUE(ReconversionStarted(started));
}

TEST(SessionCallbackTest, SelectionDeletionRangeForwardAndBackward) {
  Output forward;
  SetSelectionDeletionRange(3, &forward);
  ASSERT_TRUE(forward.has_deletion_range());
  EXPECT_EQ(forward.deletion_range().offset(), -3);
  EXPECT_EQ(forward.deletion_range().length(), 3);

  Output backward;
  SetSelectionDeletionRange(-4, &backward);
  ASSERT_TRUE(backward.has_deletion_range());
  EXPECT_EQ(backward.deletion_range().offset(), 0);
  EXPECT_EQ(backward.deletion_range().length(), 4);
}

TEST(SessionCallbackTest, SelectionDeletionRangeFailsClosed) {
  Output no_selection;
  SetSelectionDeletionRange(0, &no_selection);
  EXPECT_FALSE(no_selection.has_deletion_range());

  Output overflow;
  SetSelectionDeletionRange(std::numeric_limits<int32_t>::min(), &overflow);
  EXPECT_FALSE(overflow.has_deletion_range());

  SetSelectionDeletionRange(1, nullptr);  // Must not crash.
}

// ---------------------------------------------------------------------------
// Context loss

TEST(SessionCallbackTest, ContextLossCommandTable) {
  struct Case {
    bool has_composition;
    mozc::commands::CompositionMode mode;
    bool switching;
    SessionCommand::CommandType expected;
  };
  const Case kCases[] = {
      // A visible composition: cancelled, or committed when the user switches
      // to another input method.
      {true, mozc::commands::HIRAGANA, false, SessionCommand::REVERT},
      {true, mozc::commands::HIRAGANA, true, SessionCommand::SUBMIT},
      // No composition: confirm what was committed instead of reverting it.
      // SUBMIT would commit nothing and confirm nothing, also when the user
      // switches to another input method.
      {false, mozc::commands::HIRAGANA, false, SessionCommand::RESET_CONTEXT},
      {false, mozc::commands::HIRAGANA, true, SessionCommand::RESET_CONTEXT},
      // IME off.
      {false, mozc::commands::DIRECT, false, SessionCommand::REVERT},
      {false, mozc::commands::DIRECT, true, SessionCommand::REVERT},
  };
  for (const Case& test_case : kCases) {
    EXPECT_EQ(SelectContextLossCommand(test_case.has_composition,
                                       test_case.mode, test_case.switching),
              test_case.expected)
        << "has_composition=" << test_case.has_composition
        << " mode=" << test_case.mode << " switching=" << test_case.switching;
  }
}

TEST(SessionCallbackTest, DirectModeNeverSendsResetContext) {
  // RESET_CONTEXT moves a DIRECT session to PRECOMPOSITION: it would turn the
  // IME on behind the user's back.
  for (const bool has_composition : {false, true}) {
    for (const bool switching : {false, true}) {
      EXPECT_NE(SelectContextLossCommand(has_composition,
                                         mozc::commands::DIRECT, switching),
                SessionCommand::RESET_CONTEXT);
    }
  }
}

TEST(SessionCallbackTest, ActiveCompositionKeepsRevertAndSubmit) {
  for (const mozc::commands::CompositionMode mode :
       {mozc::commands::DIRECT, mozc::commands::HIRAGANA,
        mozc::commands::FULL_KATAKANA, mozc::commands::HALF_ASCII,
        mozc::commands::FULL_ASCII, mozc::commands::HALF_KATAKANA}) {
    EXPECT_EQ(SelectContextLossCommand(true, mode, false),
              SessionCommand::REVERT)
        << mode;
    EXPECT_EQ(SelectContextLossCommand(true, mode, true),
              SessionCommand::SUBMIT)
        << mode;
  }
}

TEST(SessionCallbackTest, EveryActiveModeConfirmsWithResetContext) {
  for (const mozc::commands::CompositionMode mode :
       {mozc::commands::HIRAGANA, mozc::commands::FULL_KATAKANA,
        mozc::commands::HALF_ASCII, mozc::commands::FULL_ASCII,
        mozc::commands::HALF_KATAKANA}) {
    for (const bool switching : {false, true}) {
      // The session is released after focus-out, so the pending learning has
      // to be confirmed whether or not the input method is being switched.
      EXPECT_EQ(SelectContextLossCommand(false, mode, switching),
                SessionCommand::RESET_CONTEXT)
          << mode << " switching=" << switching;
    }
  }
}

TEST(SessionCallbackTest, SubmitIsOnlySentForAComposition) {
  for (const mozc::commands::CompositionMode mode :
       {mozc::commands::DIRECT, mozc::commands::HIRAGANA,
        mozc::commands::FULL_KATAKANA, mozc::commands::HALF_ASCII,
        mozc::commands::FULL_ASCII, mozc::commands::HALF_KATAKANA}) {
    for (const bool switching : {false, true}) {
      EXPECT_NE(SelectContextLossCommand(false, mode, switching),
                SessionCommand::SUBMIT)
          << mode << " switching=" << switching;
    }
  }
}

// ---------------------------------------------------------------------------
// KeyEvent.mode hint

TEST(SessionCallbackTest, ModeIsAttachedOutsideDirect) {
  for (const mozc::commands::CompositionMode mode :
       {mozc::commands::HIRAGANA, mozc::commands::FULL_KATAKANA,
        mozc::commands::HALF_ASCII, mozc::commands::FULL_ASCII,
        mozc::commands::HALF_KATAKANA}) {
    KeyEvent key;
    key.set_key_code('.');
    ApplySessionModeHint(mode, &key);
    ASSERT_TRUE(key.has_mode()) << mode;
    EXPECT_EQ(key.mode(), mode);
    EXPECT_EQ(key.key_code(), '.');
  }
}

TEST(SessionCallbackTest, DirectLeavesKeyUntouched) {
  KeyEvent key;
  key.set_special_key(KeyEvent::HANKAKU);
  const std::string before = key.SerializeAsString();
  ApplySessionModeHint(mozc::commands::DIRECT, &key);
  EXPECT_FALSE(key.has_mode());
  EXPECT_EQ(key.SerializeAsString(), before);

  // Not a composition mode at all.
  ApplySessionModeHint(mozc::commands::NUM_OF_COMPOSITIONS, &key);
  EXPECT_FALSE(key.has_mode());
  EXPECT_EQ(key.SerializeAsString(), before);

  ApplySessionModeHint(mozc::commands::HIRAGANA, nullptr);  // Must not crash.
}

TEST(SessionCallbackTest, ActivatedIsNeverSet) {
  // KeyEvent.activated makes the server toggle the IME by itself.
  for (const mozc::commands::CompositionMode mode :
       {mozc::commands::DIRECT, mozc::commands::HIRAGANA,
        mozc::commands::HALF_ASCII}) {
    KeyEvent key;
    key.set_key_code('a');
    ApplySessionModeHint(mode, &key);
    EXPECT_FALSE(key.has_activated()) << mode;
  }
}

// ---------------------------------------------------------------------------
// Redraw suppression

Output LiveConversionOutput() {
  Output output;
  output.set_consumed(true);
  output.set_mode(mozc::commands::HIRAGANA);
  output.set_live_conversion(true);
  AddSegment("漢字", &output);
  return output;
}

TEST(SessionCallbackTest, SignatureIgnoresCallbackAndIds) {
  Output first = LiveConversionOutput();
  first.set_id(1);
  first.mutable_callback()->mutable_session_command()->set_type(
      SessionCommand::APPLY_ZENZ_LIVE_CORRECTION);
  first.mutable_callback()
      ->mutable_session_command()
      ->set_live_conversion_generation(1);
  first.mutable_callback()->set_delay_millisec(24);
  first.set_zenz_live_correction_debug("poll 1");

  Output second = LiveConversionOutput();
  second.set_id(2);
  second.mutable_callback()->mutable_session_command()->set_type(
      SessionCommand::APPLY_ZENZ_LIVE_CORRECTION);
  second.mutable_callback()
      ->mutable_session_command()
      ->set_live_conversion_generation(2);
  second.mutable_callback()->set_delay_millisec(24);
  second.set_zenz_live_correction_debug("poll 2");
  second.mutable_status()->set_activated(true);

  EXPECT_FALSE(DisplaySignature(first).empty());
  EXPECT_EQ(DisplaySignature(first), DisplaySignature(second));
}

TEST(SessionCallbackTest, SignatureChangesWithZenzFlags) {
  const std::string base = DisplaySignature(LiveConversionOutput());

  Output pending = LiveConversionOutput();
  pending.set_zenz_live_correction_pending(true);
  EXPECT_NE(DisplaySignature(pending), base);

  Output applied = LiveConversionOutput();
  applied.set_zenz_live_correction_applied(true);
  EXPECT_NE(DisplaySignature(applied), base);
  EXPECT_NE(DisplaySignature(applied), DisplaySignature(pending));

  Output not_live = LiveConversionOutput();
  not_live.set_live_conversion(false);
  EXPECT_NE(DisplaySignature(not_live), base);

  Output live_pending = LiveConversionOutput();
  live_pending.set_live_conversion_pending(true);
  EXPECT_NE(DisplaySignature(live_pending), base);

  Output pre_live = LiveConversionOutput();
  pre_live.set_live_conversion(false);
  pre_live.set_pre_live_conversion_reading(true);
  EXPECT_NE(DisplaySignature(pre_live), base);
  EXPECT_NE(DisplaySignature(pre_live), DisplaySignature(not_live));
}

TEST(SessionCallbackTest, SignatureChangesWithVisibleContent) {
  const std::string base = DisplaySignature(LiveConversionOutput());

  Output other_text = LiveConversionOutput();
  other_text.mutable_preedit()->mutable_segment(0)->set_value("感じ");
  EXPECT_NE(DisplaySignature(other_text), base);

  Output other_cursor = LiveConversionOutput();
  other_cursor.mutable_preedit()->set_cursor(0);
  EXPECT_NE(DisplaySignature(other_cursor), base);

  Output other_mode = LiveConversionOutput();
  other_mode.set_mode(mozc::commands::FULL_KATAKANA);
  EXPECT_NE(DisplaySignature(other_mode), base);

  Output with_candidates = LiveConversionOutput();
  with_candidates.mutable_candidate_window()->set_size(1);
  with_candidates.mutable_candidate_window()->set_position(0);
  EXPECT_NE(DisplaySignature(with_candidates), base);

  Output unconsumed = LiveConversionOutput();
  unconsumed.set_consumed(false);
  EXPECT_NE(DisplaySignature(unconsumed), base);
}

TEST(SessionCallbackTest, ResultIsHostSideEffect) {
  EXPECT_FALSE(HasHostSideEffect(LiveConversionOutput()));

  Output with_result = LiveConversionOutput();
  with_result.mutable_result()->set_type(mozc::commands::Result::STRING);
  with_result.mutable_result()->set_value("漢字");
  EXPECT_TRUE(HasHostSideEffect(with_result));
  // The committed text is not part of the signature: an Output with a side
  // effect is never skipped, whatever its signature says.
  EXPECT_EQ(DisplaySignature(with_result),
            DisplaySignature(LiveConversionOutput()));

  Output with_deletion = LiveConversionOutput();
  with_deletion.mutable_deletion_range()->set_offset(-1);
  with_deletion.mutable_deletion_range()->set_length(1);
  EXPECT_TRUE(HasHostSideEffect(with_deletion));

  Output with_url = LiveConversionOutput();
  with_url.set_url("https://example.com/");
  EXPECT_TRUE(HasHostSideEffect(with_url));

  Output with_empty_url = LiveConversionOutput();
  with_empty_url.set_url("");
  EXPECT_FALSE(HasHostSideEffect(with_empty_url));

  Output with_tool = LiveConversionOutput();
  with_tool.set_launch_tool_mode(Output::CONFIG_DIALOG);
  EXPECT_TRUE(HasHostSideEffect(with_tool));

  Output with_no_tool = LiveConversionOutput();
  with_no_tool.set_launch_tool_mode(Output::NO_TOOL);
  EXPECT_FALSE(HasHostSideEffect(with_no_tool));
}

// ---------------------------------------------------------------------------
// Logging

TEST(SessionCallbackTest, ApplicationTextIsNotLogged) {
  // ASCII on purpose: DebugString() escapes non-ASCII bytes, which would hide
  // a leak from the substring checks below.
  const std::string selection = "secret selected text";

  SessionCommand convert_reverse;
  convert_reverse.set_type(SessionCommand::CONVERT_REVERSE);
  convert_reverse.set_text(selection);
  const std::string logged_command = DebugStringForLog(convert_reverse);
  EXPECT_EQ(logged_command.find(selection), std::string::npos);
  EXPECT_NE(logged_command.find("CONVERT_REVERSE"), std::string::npos);
  EXPECT_NE(logged_command.find("redacted"), std::string::npos);
  EXPECT_NE(logged_command.find(std::to_string(selection.size())),
            std::string::npos);
  // The command itself is left alone.
  EXPECT_EQ(convert_reverse.text(), selection);

  // The reply echoes the application text in its preedit.
  Output reply;
  reply.set_consumed(true);
  AddSegment(selection, &reply);
  const std::string logged_reply =
      ReplyDebugStringForLog(convert_reverse, reply);
  EXPECT_EQ(logged_reply.find(selection), std::string::npos);
  EXPECT_NE(logged_reply.find("consumed=1"), std::string::npos);
  EXPECT_NE(logged_reply.find("preedit_segments=1"), std::string::npos);

  Output refused;
  refused.set_consumed(true);
  EXPECT_NE(ReplyDebugStringForLog(convert_reverse, refused)
                .find("preedit_segments=0"),
            std::string::npos);
}

TEST(SessionCallbackTest, CommandsWithoutTextAreLoggedInFull) {
  SessionCommand select;
  select.set_type(SessionCommand::SELECT_CANDIDATE);
  select.set_id(3);
  const std::string logged_command = DebugStringForLog(select);
  EXPECT_NE(logged_command.find("SELECT_CANDIDATE"), std::string::npos);
  EXPECT_EQ(logged_command.find("redacted"), std::string::npos);

  // The full reply, field names included, not the shape summary.
  const std::string logged_reply =
      ReplyDebugStringForLog(select, LiveConversionOutput());
  EXPECT_NE(logged_reply.find("live_conversion"), std::string::npos);
  EXPECT_NE(logged_reply.find("preedit"), std::string::npos);
  EXPECT_EQ(logged_reply.find("preedit_segments="), std::string::npos);
}

// ---------------------------------------------------------------------------
// Preedit caret

Output PreeditWithCaret(uint32_t cursor, uint32_t highlighted_position) {
  Output output;
  output.set_consumed(true);
  output.mutable_preedit()->set_cursor(cursor);
  output.mutable_preedit()->set_highlighted_position(highlighted_position);
  return output;
}

TEST(SessionCallbackTest, CaretWithoutPreeditIsZero) {
  Output output;
  output.set_live_conversion(true);
  EXPECT_EQ(PreeditCaretPosition(output), 0u);
}

TEST(SessionCallbackTest, PlainCompositionUsesEditingCaret) {
  Output output = PreeditWithCaret(4, 0);
  output.mutable_preedit()->clear_highlighted_position();
  EXPECT_EQ(PreeditCaretPosition(output), 4u);
}

TEST(SessionCallbackTest, ConventionalConversionKeepsCandidateAnchor) {
  EXPECT_EQ(PreeditCaretPosition(PreeditWithCaret(7, 2)), 2u);
}

TEST(SessionCallbackTest, LiveConversionUsesEditingCaret) {
  Output output = PreeditWithCaret(7, 0);
  output.set_live_conversion(true);
  EXPECT_EQ(PreeditCaretPosition(output), 7u);

  // A moving clause boundary does not move the caret.
  output.mutable_preedit()->set_highlighted_position(3);
  EXPECT_EQ(PreeditCaretPosition(output), 7u);

  // Not even next to a candidate list.
  output.mutable_candidate_window();
  EXPECT_EQ(PreeditCaretPosition(output), 7u);
}

TEST(SessionCallbackTest, PendingLiveConversionUsesEditingCaret) {
  Output output = PreeditWithCaret(5, 0);
  output.set_live_conversion_pending(true);
  EXPECT_EQ(PreeditCaretPosition(output), 5u);
}

TEST(SessionCallbackTest, EditingCaretAtStartIsRespectedInLiveConversion) {
  Output output = PreeditWithCaret(0, 4);
  output.set_live_conversion(true);
  EXPECT_EQ(PreeditCaretPosition(output), 0u);
}

TEST(SessionCallbackTest, ReadingFlagAloneIsNotLiveConversion) {
  Output output = PreeditWithCaret(8, 1);
  output.set_pre_live_conversion_reading(true);
  EXPECT_EQ(PreeditCaretPosition(output), 1u);
}

}  // namespace
}  // namespace fcitx
