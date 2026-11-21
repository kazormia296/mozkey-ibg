// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/session_callback.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "protocol/commands.pb.h"

namespace fcitx {
namespace {

using mozc::commands::SessionCommand;

// Commands that work on the composition or on application text.  They are
// refused in password / sensitive fields.
bool IsRefusedInSecureInput(SessionCommand::CommandType type) {
  switch (type) {
    case SessionCommand::APPLY_LIVE_CONVERSION:
    case SessionCommand::APPLY_ZENZ_LIVE_CORRECTION:
    case SessionCommand::CONVERT_REVERSE:
      return true;
    default:
      return false;
  }
}

// Same definition as Session::IsPureSpaceKey on the server.
bool IsPureSpaceKey(const mozc::commands::KeyEvent& key) {
  return !key.has_key_code() && key.modifier_keys_size() == 0 &&
         key.has_special_key() &&
         key.special_key() == mozc::commands::KeyEvent::SPACE;
}

// Number of code points in UTF-8 `text`: every byte that is not a
// continuation byte starts one.
size_t CountChars(std::string_view text) {
  size_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

// True for text that is one line without control characters.  Line breaks,
// tabs and the Unicode line / paragraph separators mark a selection that
// spans more than a phrase; the server refuses control characters anyway.
bool IsSingleLinePlainText(std::string_view text) {
  for (const unsigned char byte : text) {
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
  }
  // U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR.
  return text.find("\xE2\x80\xA8") == std::string_view::npos &&
         text.find("\xE2\x80\xA9") == std::string_view::npos;
}

}  // namespace

CallbackPlan PlanCallback(const mozc::commands::Output& output,
                          bool secure_input) {
  CallbackPlan plan;
  if (!output.has_callback() || !output.callback().has_session_command() ||
      !output.callback().session_command().has_type()) {
    return plan;
  }

  const mozc::commands::Output::Callback& callback = output.callback();
  const SessionCommand& command = callback.session_command();
  const SessionCommand::CommandType type = command.type();

  if (!IsAllowedToRun(type, secure_input)) {
    plan.action = CallbackAction::kDrop;
    return plan;
  }

  const uint32_t delay_msec =
      callback.has_delay_millisec() ? callback.delay_millisec() : 0;
  if (delay_msec > 0) {
    // Only the callback is delayed; the Output itself is applied right away.
    plan.action = CallbackAction::kSchedule;
    plan.command = command;
    plan.delay_msec = delay_msec;
    return plan;
  }

  switch (type) {
    case SessionCommand::APPLY_LIVE_CONVERSION:
    case SessionCommand::APPLY_ZENZ_LIVE_CORRECTION:
    case SessionCommand::RESUME_COMMAND_SEQUENCE:
      // Never re-enter the server from inside the handling of its own Output:
      // run these from the event loop even without a delay.
      plan.action = CallbackAction::kSchedule;
      plan.command = command;
      plan.delay_msec = 0;
      return plan;
    case SessionCommand::UNDO:
      plan.action = CallbackAction::kUndo;
      return plan;
    case SessionCommand::CONVERT_REVERSE:
      plan.action = CallbackAction::kConvertReverse;
      return plan;
    case SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE:
      plan.action = CallbackAction::kReconvertSelectionOrFallback;
      return plan;
    default:
      plan.action = CallbackAction::kDrop;
      return plan;
  }
}

bool IsAllowedToRun(SessionCommand::CommandType type, bool secure_input) {
  return !secure_input || !IsRefusedInSecureInput(type);
}

bool IsInvalidatedByKey(SessionCommand::CommandType type) {
  // Session::SendKey always advances the command sequence generation, so a
  // pending RESUME_COMMAND_SEQUENCE can no longer match.
  return type == SessionCommand::RESUME_COMMAND_SEQUENCE;
}

SelectionForReconversion ClassifySelectionForReconversion(
    std::string_view selection_text, int32_t relative_selected_length) {
  if (relative_selected_length == 0 || selection_text.empty()) {
    return SelectionForReconversion::kNone;
  }
  // Widened first: INT32_MIN cannot be negated as int32_t.
  const int64_t signed_length = relative_selected_length;
  const uint64_t selected_chars = static_cast<uint64_t>(
      signed_length < 0 ? -signed_length : signed_length);
  // The deletion range that removes the selection is derived from the
  // reported positions alone, so the reported text has to be exactly the text
  // between them.  A toolkit that reports only a window of the document can
  // cut the selection short.
  if (static_cast<uint64_t>(CountChars(selection_text)) != selected_chars) {
    return SelectionForReconversion::kNone;
  }
  if (selected_chars > kMaxReconvertSelectionChars ||
      !IsSingleLinePlainText(selection_text)) {
    return SelectionForReconversion::kNotReconvertible;
  }
  return SelectionForReconversion::kReconvertible;
}

bool IsPassThroughSpaceFallback(const mozc::commands::KeyEvent& key,
                                const mozc::commands::Output& fallback) {
  if (!IsPureSpaceKey(key)) {
    return false;
  }
  if (!fallback.consumed() || !fallback.has_result() ||
      fallback.result().type() != mozc::commands::Result::STRING ||
      fallback.result().value() != " ") {
    return false;
  }
  // Anything else the Output would apply cannot be expressed by forwarding
  // the key, so such an Output is applied as it is.
  return !fallback.has_preedit() && !fallback.has_candidate_window() &&
         !fallback.has_deletion_range();
}

void ResolveSpaceFallback(SelectionForReconversion selection,
                          const mozc::commands::KeyEvent* source_key,
                          mozc::commands::Output* fallback) {
  if (fallback == nullptr) {
    return;
  }
  // The decision is final: the callback must not be looked at again.
  fallback->clear_callback();

  if (selection != SelectionForReconversion::kNone) {
    // The user asked to reconvert a selection.  Whatever became of that
    // request, the fallback space must not replace the selected text: drop
    // everything the fallback would do to the application and keep the key.
    fallback->clear_result();
    fallback->clear_deletion_range();
    fallback->set_consumed(true);
    return;
  }

  if (source_key != nullptr &&
      IsPassThroughSpaceFallback(*source_key, *fallback)) {
    // A plain Space stays a key event the application handles itself,
    // exactly like a key Mozc did not consume.
    fallback->clear_result();
    fallback->set_consumed(false);
  }
}

bool ReconversionStarted(const mozc::commands::Output& reply) {
  return reply.consumed() && reply.has_preedit() &&
         reply.preedit().segment_size() > 0;
}

void SetSelectionDeletionRange(int32_t relative_selected_length,
                               mozc::commands::Output* output) {
  if (output == nullptr || relative_selected_length == 0 ||
      relative_selected_length == std::numeric_limits<int32_t>::min()) {
    // INT32_MIN cannot be negated.  Deleting nothing is the safe answer.
    return;
  }
  // Use DeletionRange field to remove the selected text.
  // For forward selection (that is, |relative_selected_length > 0|), the
  // offset should be a negative value to delete preceding text.
  // For backward selection (that is, |relative_selected_length < 0|),
  // IBus and/or some applications seem to expect |offset == 0| somehow.
  mozc::commands::DeletionRange* range = output->mutable_deletion_range();
  if (relative_selected_length > 0) {
    range->set_offset(-relative_selected_length);
    range->set_length(relative_selected_length);
  } else {
    range->set_offset(0);
    range->set_length(-relative_selected_length);
  }
}

SessionCommand MakeResumeCommand(uint32_t generation) {
  SessionCommand command;
  command.set_type(SessionCommand::RESUME_COMMAND_SEQUENCE);
  command.set_command_sequence_generation(generation);
  return command;
}

SessionCommand::CommandType SelectContextLossCommand(
    bool has_composition, mozc::commands::CompositionMode mode,
    bool switching_input_method) {
  if (has_composition) {
    return switching_input_method ? SessionCommand::SUBMIT
                                  : SessionCommand::REVERT;
  }
  if (mode == mozc::commands::DIRECT) {
    // RESET_CONTEXT moves a DIRECT session to PRECOMPOSITION (IME on) and
    // SUBMIT has nothing to commit.  REVERT is a no-op in DIRECT.
    return SessionCommand::REVERT;
  }
  // SUBMIT commits nothing in PRECOMPOSITION and does not confirm the pending
  // direct-commit learning or Zenz feedback.  The session can be destroyed
  // right after this command, so confirm them here in every case.
  return SessionCommand::RESET_CONTEXT;
}

void ApplySessionModeHint(mozc::commands::CompositionMode mode,
                          mozc::commands::KeyEvent* key) {
  if (key == nullptr || mode == mozc::commands::DIRECT ||
      mode >= mozc::commands::NUM_OF_COMPOSITIONS) {
    // IMEOn applies KeyEvent.mode, so a key sent while the IME is off must
    // not carry a mode of its own.
    return;
  }
  key->set_mode(mode);
}

uint32_t PreeditCaretPosition(const mozc::commands::Output& output) {
  if (!output.has_preedit()) {
    return 0;
  }
  const mozc::commands::Preedit& preedit = output.preedit();
  if (output.live_conversion() || output.live_conversion_pending()) {
    return preedit.cursor();
  }
  if (preedit.has_highlighted_position()) {
    return preedit.highlighted_position();
  }
  return preedit.cursor();
}

std::string DisplaySignature(const mozc::commands::Output& output) {
  mozc::commands::Output displayed;
  if (output.has_consumed()) {
    displayed.set_consumed(output.consumed());
  }
  if (output.has_mode()) {
    displayed.set_mode(output.mode());
  }
  if (output.has_preedit()) {
    *displayed.mutable_preedit() = output.preedit();
  }
  if (output.has_candidate_window()) {
    *displayed.mutable_candidate_window() = output.candidate_window();
  }
  if (output.live_conversion()) {
    displayed.set_live_conversion(true);
  }
  if (output.live_conversion_pending()) {
    displayed.set_live_conversion_pending(true);
  }
  if (output.zenz_live_correction_pending()) {
    displayed.set_zenz_live_correction_pending(true);
  }
  if (output.zenz_live_correction_applied()) {
    displayed.set_zenz_live_correction_applied(true);
  }
  if (output.pre_live_conversion_reading()) {
    displayed.set_pre_live_conversion_reading(true);
  }
  // Session ids, callbacks, status snapshots and debug strings never reach
  // the screen and are left out on purpose.
  return displayed.SerializePartialAsString();
}

bool HasHostSideEffect(const mozc::commands::Output& output) {
  return output.has_result() || output.has_deletion_range() ||
         (output.has_url() && !output.url().empty()) ||
         (output.has_launch_tool_mode() &&
          output.launch_tool_mode() != mozc::commands::Output::NO_TOOL);
}

std::string DebugStringForLog(const SessionCommand& command) {
  if (!command.has_text()) {
    return command.DebugString();
  }
  SessionCommand redacted = command;
  redacted.set_text("<redacted " + std::to_string(command.text().size()) +
                    " bytes>");
  return redacted.DebugString();
}

std::string ReplyDebugStringForLog(const SessionCommand& command,
                                   const mozc::commands::Output& reply) {
  if (!command.has_text()) {
    return reply.DebugString();
  }
  std::string description = "consumed=";
  description += reply.consumed() ? "1" : "0";
  description += " preedit_segments=";
  description += std::to_string(
      reply.has_preedit() ? reply.preedit().segment_size() : 0);
  description += " has_result=";
  description += reply.has_result() ? "1" : "0";
  description += " has_candidate_window=";
  description += reply.has_candidate_window() ? "1" : "0";
  description += " has_callback=";
  description += reply.has_callback() ? "1" : "0";
  return description;
}

}  // namespace fcitx
