// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#ifndef MOZC_UNIX_FCITX5_SESSION_CALLBACK_H_
#define MOZC_UNIX_FCITX5_SESSION_CALLBACK_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "protocol/commands.pb.h"

namespace fcitx {

// Fcitx-free policy for the client obligations a Mozc Output can carry.  The
// adapter (MozcState / MozcResponseParser) only executes what is decided here,
// so every rule can be unit tested without an InputContext.

enum class CallbackAction {
  // The Output carries no usable callback.
  kNone,
  // Send `command` back to the server after `delay_msec`.
  kSchedule,
  // Send UNDO immediately; the reply carries the deletion range.
  kUndo,
  // Reconvert the application selection (reconvert key).
  kConvertReverse,
  // Reconvert an explicit application selection if there is one, otherwise
  // apply the fallback already contained in the same Output.
  kReconvertSelectionOrFallback,
  // A callback this client must not execute (unknown immediate command, or a
  // command that would need application text in secure input).
  kDrop,
};

struct CallbackPlan {
  CallbackAction action = CallbackAction::kNone;
  // Valid only for kSchedule.  This is a verbatim copy of the command the
  // server asked for: generations and keys used to reject stale callbacks are
  // owned by the server and must come back unmodified.
  mozc::commands::SessionCommand command;
  uint32_t delay_msec = 0;
};

// Decides what to do with Output.callback.  A callback with a positive delay
// is always echoed verbatim, whatever its type, so that commands added by a
// newer server (RESUME_COMMAND_SEQUENCE, ...) keep working.  Without a delay
// only the commands this client knows how to execute are accepted.
// In secure input the commands that expose or act on composition/application
// text (live conversion, Zenz correction, reverse conversion) are dropped.
CallbackPlan PlanCallback(const mozc::commands::Output& output,
                          bool secure_input);

// True when a command of this type may be sent at all.  PlanCallback applies
// this rule when a callback is accepted; a delayed command is checked with it
// once more when its timer fires, because the input context can have turned
// into a password / sensitive field in between.
bool IsAllowedToRun(mozc::commands::SessionCommand::CommandType type,
                    bool secure_input);

// True when a key sent to the server makes a pending delayed command of this
// type certainly stale, so its timer can be dropped right away.  Live
// conversion and Zenz polls are deliberately NOT invalidated: the server does
// not re-attach them for no-op keys (a lone modifier, for instance) and only
// advances their timeouts when the poll arrives.
bool IsInvalidatedByKey(mozc::commands::SessionCommand::CommandType type);

// The longest application selection, in characters, that is handed to the
// converter for reverse conversion.  Reconversion is meant for a word or a
// phrase; anything longer is never sent to the server.
inline constexpr size_t kMaxReconvertSelectionChars = 256;

// What the application selection reported by the toolkit means for reverse
// conversion.
enum class SelectionForReconversion {
  // No selection, or the reported text does not cover the reported selection
  // (its positions cannot be trusted then).
  kNone,
  // A verified selection that is never sent to the server: longer than
  // kMaxReconvertSelectionChars, or not a single line of plain text.
  kNotReconvertible,
  // A verified selection that may be sent as CONVERT_REVERSE text.
  kReconvertible,
};

// `selection_text` is the selected text and `relative_selected_length` is
// cursor - anchor in characters, both as reported by the toolkit.
SelectionForReconversion ClassifySelectionForReconversion(
    std::string_view selection_text, int32_t relative_selected_length);

// RECONVERT_SELECTION_OR_INSERT_SPACE: true when `fallback` is nothing but the
// half-width space produced for a plain Space key.  Such a key is handed back
// to the application as a key event instead of being committed as text.
bool IsPassThroughSpaceFallback(const mozc::commands::KeyEvent& key,
                                const mozc::commands::Output& fallback);

// RECONVERT_SELECTION_OR_INSERT_SPACE: finishes `fallback` once it is known
// that no reconversion replaces it.  The callback is removed in every case.
// - Without a verified selection the fallback is used: a plain Space goes
//   back to the application as a key (`source_key` is the key the Output
//   answers, or nullptr when it cannot be forwarded), anything else is
//   committed as the server produced it.
// - With a verified selection the key is consumed and nothing is committed or
//   deleted, whether the reverse conversion was refused, failed or was never
//   attempted: Space must not replace the text the user asked to reconvert.
void ResolveSpaceFallback(SelectionForReconversion selection,
                          const mozc::commands::KeyEvent* source_key,
                          mozc::commands::Output* fallback);

// True when a CONVERT_REVERSE reply really started a reconversion.  A consumed
// reply without preedit means the server refused the text, in which case the
// application selection must be left alone.
bool ReconversionStarted(const mozc::commands::Output& reply);

// Adds the deletion range that removes the application selection being
// reconverted.  `relative_selected_length` is cursor - anchor in characters
// (positive: forward selection, negative: backward selection).
void SetSelectionDeletionRange(int32_t relative_selected_length,
                               mozc::commands::Output* output);

// Acknowledgement for Output.command_sequence_resume_generation.
mozc::commands::SessionCommand MakeResumeCommand(uint32_t generation);

// The command to send when the application context goes away (focus out,
// input context reset, switching to another input method).
// - A visible composition is committed (switching) or cancelled (otherwise).
// - Without a composition, REVERT would tell the server that the user undid
//   the previous commit and roll back its learning.  RESET_CONTEXT confirms it
//   instead.  This also holds when switching input methods: SUBMIT has nothing
//   to commit there and leaves the pending learning unconfirmed, although the
//   session is destroyed right afterwards.
// - In DIRECT mode nothing but REVERT (a server no-op there) is sent, because
//   RESET_CONTEXT would turn the IME on.
mozc::commands::SessionCommand::CommandType SelectContextLossCommand(
    bool has_composition, mozc::commands::CompositionMode mode,
    bool switching_input_method);

// Tells the server which composition mode the key is typed in.  The server
// disables direct commit and auto conversion for ASCII modes based on this.
// Nothing is attached in DIRECT mode, and `activated` is never set.
void ApplySessionModeHint(mozc::commands::CompositionMode mode,
                          mozc::commands::KeyEvent* key);

// The position, in characters, to report as the preedit caret.
// - During live conversion (shown or pending) the composition is still being
//   edited, so the editing caret (Preedit.cursor) is used.  The highlighted
//   segment is not the insertion point there: following it makes the caret
//   jump to the beginning whenever a delayed conversion reply arrives or a
//   clause boundary moves.  The Windows and IBus clients do the same.
// - In conventional conversion the caret also anchors the candidate list, so
//   the start of the highlighted segment is kept.
// Output.pre_live_conversion_reading alone is not live conversion.
uint32_t PreeditCaretPosition(const mozc::commands::Output& output);

// Serialized form of everything in `output` that reaches the screen.  Two
// Outputs with the same signature are drawn identically.
std::string DisplaySignature(const mozc::commands::Output& output);

// True when applying `output` changes something outside the input panel.
bool HasHostSideEffect(const mozc::commands::Output& output);

// Text for the verbose log.  SessionCommand.text carries application text
// (the selection being reconverted) and is replaced by its size.
std::string DebugStringForLog(const mozc::commands::SessionCommand& command);

// Text for the verbose log of the reply to `command`.  The reply to a command
// that carried application text echoes that text in its preedit, so only its
// shape is described.
std::string ReplyDebugStringForLog(
    const mozc::commands::SessionCommand& command,
    const mozc::commands::Output& reply);

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX5_SESSION_CALLBACK_H_
