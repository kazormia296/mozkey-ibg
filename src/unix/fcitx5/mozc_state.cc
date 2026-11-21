// Copyright 2012~2013, Weng Xuetian <wengxt@gmail.com>
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

#include "unix/fcitx5/mozc_state.h"

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "base/process.h"
#include "base/vlog.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "unix/fcitx5/display_status.h"
#include "unix/fcitx5/fcitx_key_event_handler.h"
#include "unix/fcitx5/grimodex_context.h"
#include "unix/fcitx5/i18nwrapper.h"
#include "unix/fcitx5/mozc_engine.h"
#include "unix/fcitx5/mozc_response_parser.h"
#include "unix/fcitx5/session_callback.h"
#include "unix/fcitx5/surrounding_text_util.h"

namespace fcitx {
namespace {

mozc::commands::SessionCommand BuildCompositionModeCommand(
    mozc::commands::CompositionMode mode,
    mozc::commands::CompositionMode current_mode) {
  mozc::commands::SessionCommand command;
  if (mode == mozc::commands::DIRECT) {
    command.set_type(mozc::commands::SessionCommand::TURN_OFF_IME);
    command.set_composition_mode(current_mode);
  } else {
    command.set_type(mozc::commands::SessionCommand::SWITCH_COMPOSITION_MODE);
    command.set_composition_mode(mode);
  }
  return command;
}

// While the usage panel is shown the context is modal: keys are swallowed and
// the server is not contacted.  A delayed command that comes due in that time
// is looked at again after this delay instead of being sent.
constexpr uint32_t kUsagePanelRetryDelayMsec = 200;

}  // namespace

MozcState::MozcState(InputContext* ic, MozcEngine* engine)
    : ic_(ic),
      engine_(engine),
      raw_reading_recovery_(
          [this](MozcClientInterface* client, uint64_t generation,
                 const mozc::commands::Context& context) {
            return InitializeSessionGeneration(client, generation, context);
          },
          [this](const mozc::commands::Output* replayed_output,
                 mozc::commands::KeyEvent* key) {
            // Runs right before each send, after a replacement session got
            // its initial mode.  When the reading was replayed into that
            // session, the key continues the replayed composition in the
            // replayed mode; composition_mode_ catches up with it when the
            // reply to this key is parsed.
            const mozc::commands::CompositionMode mode =
                (replayed_output != nullptr && replayed_output->has_mode())
                    ? replayed_output->mode()
                    : composition_mode_;
            ApplySessionModeHint(mode, key);
          }),
      handler_(std::make_unique<KeyEventHandler>()) {
  // mozc::Logging::SetVerboseLevel(1);
  MOZC_VLOG(1) << "MozcState created.";
}

MozcState::~MozcState() { MOZC_VLOG(1) << "MozcState destroyed."; }

void MozcState::UpdatePreeditMethod() {
  mozc::config::Config config;
  if (!GetClient()->GetConfig(&config)) {
    LOG(ERROR) << "GetConfig failed";
    return;
  }
  preedit_method_ = config.has_preedit_method() ? config.preedit_method()
                                                : mozc::config::Config::ROMAN;
  show_live_conversion_reading_ = config.show_live_conversion_ruby_window();
  show_conversion_status_ = config.show_conversion_status_indicator();
  conversion_status_label_style_ =
      config.conversion_status_indicator_style().label();
  std::string error;
  mozc::commands::Output raw_response;
  mozc::commands::CompositionMode mode = composition_mode_;
  if (TrySendCommand(mozc::commands::SessionCommand::GET_STATUS, &raw_response,
                     &error)) {
    if (raw_response.has_status()) {
      if (raw_response.status().activated()) {
        mode = raw_response.status().mode();
      } else {
        mode = mozc::commands::DIRECT;
      }
    }
  }
  if (mode != composition_mode_) {
    composition_mode_ = mode;
    engine_->compositionModeUpdated(ic_);
  }
}

bool MozcState::TrySendKeyEvent(InputContext* ic,
                                const mozc::commands::KeyEvent& event,
                                mozc::commands::Output* out,
                                mozc::commands::KeyEvent* sent_event,
                                std::string* out_error) const {
  DCHECK(out);
  DCHECK(sent_event);
  DCHECK(out_error);

  // Call EnsureConnection just in case MozcState::MozcConnection() fails
  // to establish the server connection.
  auto* client = GetClient();
  if (!client->EnsureConnection()) {
    *out_error = "EnsureConnection failed";
    MOZC_VLOG(1) << "EnsureConnection failed";
    return false;
  }

  if ((composition_mode_ == mozc::commands::DIRECT) &&
      !client->IsDirectModeCommand(event)) {
    MOZC_VLOG(1) << "In DIRECT mode. Not consumed.";
    return false;  // not consumed.
  }

  const mozc::commands::Context context =
      BuildContext(/*include_surrounding_text=*/true);

  // Pending live-conversion and Zenz callbacks stay armed: the server does not
  // re-attach them for a key it ignores (a lone modifier, for instance) and a
  // stale one is answered with the current state.  Only the commands this key
  // certainly invalidates on the server are dropped.
  CancelDelayedSessionCommandsInvalidatedByKey();

  // The composition mode the key is typed in is not attached here: the
  // dispatch can find a replaced session, whose initial mode and replayed
  // reading decide it.  The key finalizer given to RawReadingRecovery stamps
  // each copy right before it is sent, and `sent_event` is what the server
  // really received.
  MOZC_VLOG(1) << "TrySendKeyEvent: " << event.DebugString();
  if (!raw_reading_recovery_.DispatchKey(client, event, context,
                                         IsSecureInput(), out, sent_event)) {
    *out_error = "SendKey failed";
    MOZC_VLOG(1) << "ERROR";
    return false;
  }
  MOZC_VLOG(1) << "OK: " << out->DebugString();
  return true;
}

bool MozcState::TrySendClick(int32_t unique_id,
                             mozc::commands::SessionCommand::CommandType type,
                             mozc::commands::Output* out,
                             std::string* out_error) const {
  DCHECK(out);
  DCHECK(out_error);

  mozc::commands::SessionCommand command;
  command.set_type(type);
  command.set_id(unique_id);
  return TrySendRawCommand(command, out, out_error);
}

bool MozcState::TrySendCompositionMode(mozc::commands::CompositionMode mode,
                                       mozc::commands::Output* out,
                                       std::string* out_error) const {
  DCHECK(out);
  DCHECK(out_error);

  const mozc::commands::SessionCommand command =
      BuildCompositionModeCommand(mode, composition_mode_);
  return TrySendRawCommand(command, out, out_error);
}

bool MozcState::TrySendCommand(mozc::commands::SessionCommand::CommandType type,
                               mozc::commands::Output* out,
                               std::string* out_error) const {
  DCHECK(out);
  DCHECK(out_error);

  mozc::commands::SessionCommand command;
  command.set_type(type);
  return TrySendRawCommand(command, out, out_error);
}

bool MozcState::TrySendRawCommand(const mozc::commands::SessionCommand& command,
                                  mozc::commands::Output* out,
                                  std::string* out_error) const {
  // Application text (a selection being reconverted) never reaches the log.
  MOZC_VLOG(1) << "TrySendRawCommand: " << DebugStringForLog(command);
  const mozc::commands::Context context =
      BuildContext(/*include_surrounding_text=*/false);
  auto* client = GetClient();
  mozc::commands::Output recovered_output;
  const RawReadingRecovery::PrepareResult prepare_result =
      raw_reading_recovery_.Prepare(client, context, IsSecureInput(),
                                    &recovered_output);
  if (prepare_result == RawReadingRecovery::PrepareResult::kFailed) {
    *out_error = "EnsureSession/recovery failed";
    return false;
  }
  if (prepare_result ==
      RawReadingRecovery::PrepareResult::kSessionChanged) {
    // This command belongs to the destroyed session.  A newly reconstructed
    // preedit can be rendered, but the candidate/callback/effect command itself
    // must never cross the generation boundary.
    if (recovered_output.has_consumed()) {
      *out = std::move(recovered_output);
      return true;
    }
    *out_error = "Stale session command dropped";
    return false;
  }
  if (!SendCommandWithGrimodexContext(client, command, context, out)) {
    // A failure can discover and recreate a dead server inside Client.  Recover
    // the reading for UI continuity, but never retry the SessionCommand.
    recovered_output.Clear();
    const RawReadingRecovery::PrepareResult recovery_result =
        raw_reading_recovery_.Prepare(client, context, IsSecureInput(),
                                      &recovered_output);
    if (recovery_result ==
            RawReadingRecovery::PrepareResult::kSessionChanged &&
        recovered_output.has_consumed()) {
      *out = std::move(recovered_output);
      return true;
    }
    *out_error = "SendCommand failed";
    MOZC_VLOG(1) << "ERROR";
    return false;
  }
  MOZC_VLOG(1) << "OK: " << ReplyDebugStringForLog(command, *out);
  return true;
}

// This function is called when users press or release a key.
bool MozcState::ProcessKeyEvent(KeySym sym, uint32_t keycode, KeyStates state,
                                bool layout_is_jp, bool is_key_up) {
  auto normalized_key = Key(sym, state).normalize();
  if (displayUsage_) {
    if (is_key_up) {
      return true;
    }

    if (normalized_key.check(Key(FcitxKey_Escape))) {
      displayUsage_ = false;
      ProcessKeyEvent(FcitxKey_VoidSymbol, 0, KeyState::NoState, layout_is_jp,
                      false);
    }
    return true;
  }

  if (*engine_->config().expandMode == ExpandMode::Hotkey &&
      normalized_key.check(*engine_->config().expand)) {
    if (!title_.empty() || !description_.empty()) {
      DisplayUsage();
      return true;
    }
  }

  if (!ApplyInitialModeForCurrentClient()) {
    return false;
  }

  mozc::commands::KeyEvent event;
  std::optional<std::string> compose;
  do {
    if (!is_key_up &&
        !state.testAny(KeyStates{KeyState::Ctrl, KeyState::Super})) {
      compose = engine_->instance()->processComposeString(ic_, sym);
      if (!compose) {
        return true;
      }
      if (!compose->empty()) {
        auto length = utf8::lengthValidated(*compose);
        if (length == utf8::INVALID_LENGTH) {
          return true;
        }
        if (!handler_->GetKeyEvent(*compose, preedit_method_, layout_is_jp,
                                   &event)) {
          return false;
        }
        break;
      }
    }
    if (!handler_->GetKeyEvent(sym, keycode, state, preedit_method_,
                               layout_is_jp, is_key_up, &event)) {
      return false;
    }
  } while (false);

  std::string error;
  mozc::commands::Output raw_response;
  // `event` as the server received it, i.e. with the composition mode of the
  // session it went to.
  mozc::commands::KeyEvent sent_event;
  if (!TrySendKeyEvent(ic_, event, &raw_response, &sent_event, &error)) {
    // TODO(yusukes): Show |error|.
    if (compose && !compose->empty()) {
      ic_->commitString(*compose);
      Reset();
      return true;
    }
    return false;  // not consumed.
  }

  // The journal replays exactly what was sent, composition mode included.
  raw_reading_recovery_.RecordSuccessfulKey(sent_event, raw_response,
                                             IsSecureInput());

  // A composed string is not the physical key Fcitx would forward, so it can
  // never be handed back to the application as a key event.
  const bool composed = compose && !compose->empty();
  return ParseResponse(std::move(raw_response), composed ? nullptr : &event);
}

// This function is called from SCIM framework when users click the candidate
// window.
void MozcState::SelectCandidate(int32_t id, bool submit) {
  if (id == kBadCandidateId) {
    LOG(ERROR) << "The clicked candidate doesn't have unique ID.";
    return;
  }
  MOZC_VLOG(1) << "select_candidate, id=" << id << " submit=" << submit;

  std::string error;
  mozc::commands::Output raw_response;
  const mozc::commands::SessionCommand::CommandType type =
      submit ? mozc::commands::SessionCommand::SUBMIT_CANDIDATE
             : mozc::commands::SessionCommand::SELECT_CANDIDATE;
  if (!TrySendClick(id, type, &raw_response, &error)) {
    LOG(ERROR) << "IPC failed. error=" << error;
    last_display_signature_.clear();
    SetAuxString(error);
    DrawAll();
  } else {
    ParseResponse(std::move(raw_response));
  }
}

// This function is called from SCIM framework.
void MozcState::Reset() {
  MOZC_VLOG(1) << "resetim";
  raw_reading_recovery_.ClearReading();
  CancelDelayedSessionCommands();
  last_display_signature_.clear();
  std::string error;
  mozc::commands::Output raw_response;
  const mozc::commands::SessionCommand::CommandType command =
      SelectContextLossCommand(HasVisibleComposition(), composition_mode_,
                               /*switching_input_method=*/false);
  if (TrySendCommand(command, &raw_response, &error)) {
    engine_->parser()->ParseResponse(raw_response, ic_);
  }
  // The reply must not arm a callback for a context that was just reset.
  CancelDelayedSessionCommands();
  ClearAll();  // just in case.
  DrawAll();
  engine_->instance()->resetCompose(ic_);
}

bool MozcState::Paging(bool prev) {
  MOZC_VLOG(1) << "paging";
  // The parser redraws the candidate list without going through
  // MozcState::ParseResponse.
  last_display_signature_.clear();
  std::string error;
  mozc::commands::SessionCommand::CommandType command =
      prev ? mozc::commands::SessionCommand::CONVERT_PREV_PAGE
           : mozc::commands::SessionCommand::CONVERT_NEXT_PAGE;
  mozc::commands::Output raw_response;
  if (TrySendCommand(command, &raw_response, &error)) {
    engine_->parser()->ParseResponse(raw_response, ic_);
    return true;
  }
  return false;
}

// This function is called when the ic gets focus.
void MozcState::FocusIn() {
  MOZC_VLOG(1) << "MozcState::FocusIn()";

  raw_reading_recovery_.ResetSessionBoundary();
  AdvanceFocusEpoch();
  last_display_signature_.clear();
  handler_->Clear();
  UpdatePreeditMethod();
  ApplyInitialModeForCurrentClient();
  DrawAll();
}

bool MozcState::ApplyInitialModeForCurrentClient() {
  MozcClientInterface* client = GetClient();
  // Initial mode is part of creating a replacement session, so it must reach
  // that session before RawReadingRecovery replays any unfinished reading.
  // Sending this through TrySendRawCommand would run recovery first and a mode
  // switch could then commit the recovered preedit as a separate result.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!client->EnsureSession()) {
      continue;
    }
    const uint64_t generation_before = client->session_generation();
    if (generation_before == 0) {
      continue;
    }
    if (initial_mode_client_generation_ == generation_before) {
      return true;
    }

    const mozc::commands::Context context =
        BuildContext(/*include_surrounding_text=*/false);
    if (InitializeSessionGeneration(client, generation_before, context)) {
      return true;
    }
    MOZC_VLOG(1) << "Applying the initial composition mode attempt failed";
  }

  // Construction happens while InputContext's derived frontend can still be
  // incomplete.  FocusIn is the first lifecycle point where BuildContext may
  // call its virtual frontend() method safely.  Leave this pending after an
  // IPC failure so FocusIn, a capability transition, or the next key retries
  // the setting without processing that key in the wrong mode.
  return false;
}

bool MozcState::InitializeSessionGeneration(
    MozcClientInterface* client, uint64_t generation,
    const mozc::commands::Context& context) {
  if (client == nullptr || generation == 0 ||
      client->session_generation() != generation) {
    return false;
  }
  if (initial_mode_client_generation_ == generation) {
    return true;
  }

  const mozc::commands::SessionCommand command = BuildCompositionModeCommand(
      *engine_->config().initialMode, composition_mode_);
  mozc::commands::Output raw_response;
  if (!SendCommandWithGrimodexContext(client, command, context,
                                      &raw_response) ||
      client->session_generation() != generation ||
      !raw_response.has_mode()) {
    return false;
  }

  initial_mode_client_generation_ = generation;
  SetCompositionMode(raw_response.mode(), /*updateUI=*/true);
  return true;
}

// This function is called when the ic loses focus.
void MozcState::FocusOut(const InputContextEvent& event) {
  MOZC_VLOG(1) << "MozcState::FocusOut()";
  raw_reading_recovery_.ClearReading();
  CancelDelayedSessionCommands();
  last_display_signature_.clear();
  std::string error;
  mozc::commands::Output raw_response;

  const mozc::commands::SessionCommand::CommandType command =
      SelectContextLossCommand(
          HasVisibleComposition(), composition_mode_,
          event.type() == EventType::InputContextSwitchInputMethod);

  if (TrySendCommand(command, &raw_response, &error)) {
    engine_->parser()->ParseResponse(raw_response, ic_);
  }
  // The reply must not arm a callback for a context that lost focus.
  CancelDelayedSessionCommands();
  ClearAll();  // just in case.
  DrawAll();
  engine_->instance()->resetCompose(ic_);
  handler_->Clear();
  displayUsage_ = false;
  ReleaseClient();
}

void MozcState::CapabilityAboutToChange() {
  MOZC_VLOG(1) << "MozcState::CapabilityAboutToChange()";
  raw_reading_recovery_.ResetSessionBoundary();
  AdvanceFocusEpoch();
  CancelDelayedSessionCommands();
  last_display_signature_.clear();

  // This event is delivered before Fcitx installs the new capability flags.
  // Do not send a final command with the old domain.  Destroy the uniquely
  // owned session first, then purge every adapter-side reference and timer.
  ReleaseClient();
  displayUsage_ = false;
  title_.clear();
  description_.clear();
  handler_->Clear();
  ClearAll();
  DrawAll();
  engine_->instance()->resetCompose(ic_);
}

void MozcState::CapabilityChanged() {
  MOZC_VLOG(1) << "MozcState::CapabilityChanged()";
  // The old session was destroyed in CapabilityAboutToChange.  Create the
  // replacement only after Fcitx has installed the new security-domain flags,
  // so its first command already carries the new typed context.
  UpdatePreeditMethod();
  ApplyInitialModeForCurrentClient();
  DrawAll();
}

bool MozcState::IsSecureInput() const {
  return ic_->capabilityFlags().test(CapabilityFlag::PasswordOrSensitive);
}

bool MozcState::ParseResponse(mozc::commands::Output raw_response,
                              const mozc::commands::KeyEvent* source_key) {
  auto oldMode = composition_mode_;
  // Decide between reconversion and the fallback before any part of the
  // fallback (its committed space in particular) is applied.
  ResolveReconvertSelectionOrFallback(source_key, &raw_response);
  const CallbackAction callback_action =
      PlanCallback(raw_response, IsSecureInput()).action;
  // The panel is redrawn from this response, which takes the usage panel
  // away.  Keys are swallowed for as long as displayUsage_ is set, so it must
  // never outlive the panel it stands for.
  displayUsage_ = false;
  ClearAll();
  const bool consumed = engine_->parser()->ParseResponse(raw_response, ic_);
  if (!consumed) {
    MOZC_VLOG(1) << "The input was not consumed by Mozc.";
  }
  OpenUrl();
  DrawAll();
  if (callback_action == CallbackAction::kUndo ||
      callback_action == CallbackAction::kConvertReverse) {
    // The parser followed the callback and drew the reply to it, which is
    // not `raw_response`.
    last_display_signature_.clear();
  } else {
    last_display_signature_ = DisplaySignature(raw_response);
  }
  if (oldMode != composition_mode_ && aux_.empty() && preedit_.empty() &&
      !ic_->inputPanel().candidateList()) {
    engine_->instance()->showInputMethodInformation(ic_);
  }
  return consumed;
}

void MozcState::ResolveReconvertSelectionOrFallback(
    const mozc::commands::KeyEvent* source_key,
    mozc::commands::Output* response) {
  if (PlanCallback(*response, IsSecureInput()).action !=
      CallbackAction::kReconvertSelectionOrFallback) {
    return;
  }
  // The decision is taken exactly once, here: every path below ends with the
  // callback removed from `response`.

  // Application text is never read in password / sensitive fields, so no
  // selection is known there and the fallback is used.
  SelectionForReconversion selection = SelectionForReconversion::kNone;
  if (!IsSecureInput()) {
    // Only a selection the toolkit itself reports counts.  Passing no
    // clipboard addon disables the PRIMARY-selection guess, which could match
    // text the user selected earlier somewhere else.
    SurroundingTextInfo info;
    if (GetSurroundingText(ic_, &info, /*clipboard=*/nullptr)) {
      selection = ClassifySelectionForReconversion(
          info.selection_text, info.relative_selected_length);
    }
    if (selection == SelectionForReconversion::kReconvertible) {
      mozc::commands::Output reply;
      if (ReverseConvertSelection(info, &reply) ==
          ReverseConversionResult::kStarted) {
        // The reconversion replaces the fallback, including its space.
        *response = std::move(reply);
        return;
      }
    }
  }

  // No reconversion.  Without a selection the fallback is used (a plain Space
  // goes back to the application as a key).  With a selection the key is
  // consumed and nothing else happens: the server refused the text, the
  // command failed, or the selection is too large to be sent at all, and in
  // none of these cases may Space replace what the user selected.
  ResolveSpaceFallback(selection, source_key, response);
}

MozcState::ReverseConversionResult MozcState::ReverseConvertSelection(
    const SurroundingTextInfo& selection, mozc::commands::Output* reply) {
  DCHECK(reply);
  reply->Clear();
  // Callers check both conditions; they are repeated at the one place that
  // hands application text to the server.
  if (IsSecureInput() ||
      ClassifySelectionForReconversion(selection.selection_text,
                                       selection.relative_selected_length) !=
          SelectionForReconversion::kReconvertible) {
    return ReverseConversionResult::kNotSent;
  }

  mozc::commands::SessionCommand convert_reverse;
  convert_reverse.set_type(mozc::commands::SessionCommand::CONVERT_REVERSE);
  convert_reverse.set_text(selection.selection_text);

  // The reply is accepted only from the session the request was made for.  If
  // the session was replaced, the command was dropped and `reply` is at most
  // the reading replayed into the replacement session.
  const uint64_t generation = GetClient()->session_generation();
  if (generation == 0 || !SendCommand(convert_reverse, reply)) {
    reply->Clear();
    return ReverseConversionResult::kNotSent;
  }
  if (GetClient()->session_generation() != generation ||
      !ReconversionStarted(*reply)) {
    return ReverseConversionResult::kRefused;
  }

  if (PlanCallback(*reply, IsSecureInput()).action !=
      CallbackAction::kSchedule) {
    // Immediate callbacks are not followed recursively.
    reply->clear_callback();
  }
  // Remove the selected text as the first step of the reconversion.
  SetSelectionDeletionRange(selection.relative_selected_length, reply);
  // The reconverted composition was not typed and cannot be replayed.
  MarkCompositionNotReplayable();
  if (reply->has_command_sequence_resume_generation()) {
    // A keymap macro is suspended behind this reconversion.  Tell the server
    // once the reconverted preedit has been drawn; the 1 ms one-shot runs
    // after the current (synchronous) redraw.
    ScheduleDelayedSessionCommand(
        MakeResumeCommand(reply->command_sequence_resume_generation()), 1);
  }
  return ReverseConversionResult::kStarted;
}

void MozcState::HandleDelayedCommandResponse(
    mozc::commands::Output response) {
  const CallbackPlan plan = PlanCallback(response, IsSecureInput());
  // A callback that needs more than a timer always takes the full path.
  const bool passive_callback = plan.action == CallbackAction::kNone ||
                                plan.action == CallbackAction::kSchedule ||
                                plan.action == CallbackAction::kDrop;
  if (passive_callback && !last_display_signature_.empty() &&
      !HasHostSideEffect(response) &&
      DisplaySignature(response) == last_display_signature_) {
    // Nothing on the screen would change (a Zenz poll that is still waiting,
    // or the answer to a stale callback).  Do not push the same preedit and
    // panel to the application again; only keep the callback chain alive.
    if (plan.action == CallbackAction::kSchedule) {
      ScheduleDelayedSessionCommand(plan.command, plan.delay_msec);
    }
    return;
  }
  ParseResponse(std::move(response));
}

bool MozcState::HasVisibleComposition() const {
  return !preedit_.empty() || ic_->inputPanel().candidateList() != nullptr;
}

void MozcState::MarkCompositionNotReplayable() {
  raw_reading_recovery_.MarkCompositionNotReplayable();
}

void MozcState::SetResultString(const std::string& result_string) {
  raw_reading_recovery_.ClearReading();
  ic_->commitString(result_string);
}

void MozcState::SetPreeditInfo(Text preedit_info) {
  preedit_ = std::move(preedit_info);
}

void MozcState::SetAuxString(const std::string& str) { aux_ = str; }

void MozcState::SetLiveConversionReading(const std::string& reading,
                                         bool before_live_conversion) {
  live_conversion_reading_ = reading;
  reading_is_before_live_conversion_ = before_live_conversion;
}

void MozcState::SetConversionStatusLabel(const std::string& label) {
  conversion_status_label_ = label;
}

void MozcState::SetCompositionMode(mozc::commands::CompositionMode mode,
                                   bool updateUI) {
  composition_mode_ = mode;
  DCHECK(composition_mode_ < mozc::commands::NUM_OF_COMPOSITIONS);
  if (updateUI) {
    engine_->compositionModeUpdated(ic_);
  }
}

void MozcState::SendCompositionMode(mozc::commands::CompositionMode mode) {
  // Send the SWITCH_COMPOSITION_MODE command.
  last_display_signature_.clear();
  std::string error;
  mozc::commands::Output raw_response;
  if (TrySendCompositionMode(mode, &raw_response, &error)) {
    engine_->parser()->ParseResponse(raw_response, ic_);
  }
}

void MozcState::SetUrl(const std::string& url) { url_ = url; }

void MozcState::ClearAll() {
  // Pending delayed commands are deliberately left alone: this runs for every
  // response, and a redraw says nothing about their validity.
  SetPreeditInfo(Text());
  SetAuxString("");
  SetLiveConversionReading("");
  SetConversionStatusLabel("");
  ic_->inputPanel().reset();
  url_.clear();
}

void MozcState::DrawAll() {
  const std::string reading =
      show_live_conversion_reading_ ? live_conversion_reading_ : std::string();
  const bool client_preedit =
      ic_->capabilityFlags().test(CapabilityFlag::Preedit);
  // The label is on only when both the Mozc setting shared with the other
  // platforms and the Fcitx option allow it, and only next to something the
  // panel shows anyway: a user who turned the reading off must not get a
  // popup that says nothing but "Mozc" while typing.
  const std::string status_label =
      (show_conversion_status_ && *engine_->config().showConversionStatus &&
       StatusLabelHasHost(!reading.empty(), !aux_.empty(),
                          /*panel_shows_preedit=*/!client_preedit &&
                              !preedit_.empty()))
          ? conversion_status_label_
          : std::string();
  const std::string aux = BuildAuxText(reading, status_label, aux_);
  if (client_preedit) {
    Text preedit = preedit_;
    if (*engine_->config().preeditCursorPositionAtBeginning) {
      preedit.setCursor(0);
    }
    ic_->inputPanel().setClientPreedit(preedit);
    if (!aux.empty()) {
      ic_->inputPanel().setAuxUp(Text(aux));
    }
  } else {
    Text preedit = preedit_;
    if (!preedit.empty()) {
      // Before live conversion starts the preedit shown in the panel is the
      // reading itself; do not print the same kana twice on one line.
      const bool duplicate_reading = reading_is_before_live_conversion_ &&
                                     reading == preedit_.toString();
      const std::string panel_aux =
          duplicate_reading ? BuildAuxText("", status_label, aux_) : aux;
      if (!panel_aux.empty()) {
        preedit.append(" ");
        preedit.append(panel_aux);
      }
      ic_->inputPanel().setPreedit(preedit);
    } else if (!aux.empty()) {
      ic_->inputPanel().setAuxUp(Text(aux));
    }
  }
  ic_->updatePreedit();
  ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void MozcState::OpenUrl() {
  if (url_.empty()) {
    return;
  }
  mozc::Process::OpenBrowser(url_);
  url_.clear();
}

bool MozcState::SendCommand(
    const mozc::commands::SessionCommand& session_command,
    mozc::commands::Output* new_output) {
  std::string error;
  return TrySendRawCommand(session_command, new_output, &error);
}

void MozcState::SetUsage(const std::string& title,
                         const std::string& description) {
  title_ = title;
  description_ = description;
}

void MozcState::DisplayUsage() {
  displayUsage_ = true;
  last_display_signature_.clear();

  ic_->inputPanel().reset();
  auto candidateList = std::make_unique<DisplayOnlyCandidateList>();

  auto lines = stringutils::split(description_, "\n");
  candidateList->setLayoutHint(CandidateLayoutHint::Vertical);
  candidateList->setContent(lines);
  ic_->inputPanel().setCandidateList(std::move(candidateList));
  auto str = title_ + " [" + _("Press Escape to go back") + "]";
  ic_->inputPanel().setAuxUp(Text(str));
  ic_->updatePreedit();
  ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

MozcClientInterface* MozcState::GetClient() const {
  if (!client_) {
    client_ = engine_->pool()->requestClient(ic_->uuid());
  }
  return client_.get();
}

void MozcState::ReleaseClient() {
  // A delayed command belongs to the session that asked for it.
  CancelDelayedSessionCommands();
  raw_reading_recovery_.ResetSessionBoundary();
  client_.reset();
  initial_mode_client_generation_ = 0;
}

mozc::commands::Context MozcState::BuildContext(
    bool include_surrounding_text) const {
  GrimodexSurroundingTextProvider surrounding_text_provider;
  if (include_surrounding_text) {
    surrounding_text_provider = [this]()
        -> std::optional<GrimodexSurroundingText> {
      SurroundingTextInfo info;
      if (!GetSurroundingText(ic_, &info, engine_->clipboardAddon())) {
        return std::nullopt;
      }
      return GrimodexSurroundingText{
          .preceding_text = std::move(info.preceding_text),
          .following_text = std::move(info.following_text),
      };
    };
  }

  const char* frontend = ic_->frontend();
  return BuildGrimodexContext(
      ic_->program(), frontend == nullptr ? std::string_view() : frontend,
      IsSecureInput(), focus_epoch_, surrounding_text_provider);
}

void MozcState::AdvanceFocusEpoch() {
  focus_epoch_ = AdvanceGrimodexFocusEpoch(focus_epoch_);
}

void MozcState::ScheduleDelayedSessionCommand(
    const mozc::commands::SessionCommand& command, uint32_t delay_msec) {
  if (!command.has_type()) {
    return;
  }
  const int type = command.type();
  // Same type: the newest request wins.  Other types keep their own timers.
  delayed_commands_.erase(type);
  const uint64_t target_usec =
      fcitx::now(CLOCK_MONOTONIC) + uint64_t{delay_msec} * 1000;
  const uint64_t focus_epoch = focus_epoch_;
  delayed_commands_[type] = engine_->instance()->eventLoop().addTimeEvent(
      CLOCK_MONOTONIC, target_usec, 0,
      [this, type, command, focus_epoch](fcitx::EventSourceTime* /*timer*/,
                                         uint64_t /*usec*/) {
        // Take the timer out of the table first: it keeps the running timer
        // alive until this handler returns, and lets the reply schedule the
        // next command of the same type.  Declared first, destroyed last.
        // The entry found here is the timer that fired: replacing or
        // cancelling a request destroys its timer, which then cannot fire.
        std::unique_ptr<fcitx::EventSourceTime> timer_keeper;
        const auto it = this->delayed_commands_.find(type);
        if (it == this->delayed_commands_.end()) {
          return false;
        }
        timer_keeper = std::move(it->second);
        this->delayed_commands_.erase(it);

        // The closure is owned by the timer; work on a copy of its state.
        const mozc::commands::SessionCommand pending_command = command;
        if (focus_epoch != this->focus_epoch_) {
          // Scheduled for an earlier focus / input domain of this context.
          return false;
        }
        // Whether the command may run was decided when it was scheduled.  Do
        // not rely on every focus and capability transition having cancelled
        // it since: check the context once more as it is right now.
        if (!this->ic_->hasFocus() ||
            !IsAllowedToRun(pending_command.type(), this->IsSecureInput())) {
          return false;
        }
        if (this->displayUsage_) {
          // The reply would replace the usage panel while keys are still
          // swallowed for it.  Keep the request pending until the panel is
          // closed.
          this->ScheduleDelayedSessionCommand(pending_command,
                                              kUsagePanelRetryDelayMsec);
          return false;
        }
        mozc::commands::Output new_output;
        if (this->SendCommand(pending_command, &new_output)) {
          this->HandleDelayedCommandResponse(std::move(new_output));
        }
        return false;
      });
}

void MozcState::CancelDelayedSessionCommands() { delayed_commands_.clear(); }

void MozcState::CancelDelayedSessionCommandsInvalidatedByKey() const {
  for (auto it = delayed_commands_.begin(); it != delayed_commands_.end();) {
    if (IsInvalidatedByKey(
            static_cast<mozc::commands::SessionCommand::CommandType>(
                it->first))) {
      it = delayed_commands_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace fcitx
