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

#ifndef MOZC_UNIX_FCITX_FCITX_MOZC_H_
#define MOZC_UNIX_FCITX_FCITX_MOZC_H_

#include <fcitx-utils/key.h>
#include <fcitx/event.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/text.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <fcitx-utils/event.h>

#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "unix/fcitx5/mozc_client_interface.h"
#include "unix/fcitx5/raw_reading_recovery.h"

namespace fcitx {
inline const int32_t kBadCandidateId = -12345;
class MozcConnectionInterface;
class MozcResponseParser;
class KeyTranslator;
class KeyEventHandler;
class MozcEngine;
struct SurroundingTextInfo;

class MozcState : public InputContextProperty {
 public:
  // Outcome of ReverseConvertSelection().
  enum class ReverseConversionResult {
    // Nothing reached the server, or it gave no answer.  The reply is empty.
    kNotSent,
    // The server answered without starting a reconversion (it refuses text it
    // cannot reverse-convert), or the answer came from a replacement session
    // that never saw the request.  The application text must stay untouched.
    kRefused,
    // The reply holds the reconverted composition and removes the selection.
    kStarted,
  };

  // This constructor is used by unittests.
  MozcState(InputContext* ic, MozcEngine* engine);
  MozcState(const MozcState&) = delete;
  virtual ~MozcState();

  void UpdatePreeditMethod();

  bool ProcessKeyEvent(KeySym sym, uint32_t keycode, KeyStates state,
                       bool layout_is_jp, bool is_key_up);
  // Called when a candidate is clicked.  A passive suggestion is committed
  // (`submit`); any other candidate is only selected.
  void SelectCandidate(int32_t id, bool submit = false);
  void Reset();
  void FocusIn();
  void FocusOut(const InputContextEvent& event);
  void CapabilityAboutToChange();
  void CapabilityChanged();
  bool Paging(bool prev);

  bool IsSecureInput() const;

  // Functions called by the MozcResponseParser class to update UI.

  // Displays a 'result' (aka 'commit string') on FCITX UI.
  void SetResultString(const std::string& result_string);
  // Displays a 'preedit' string on FCITX UI. This function takes ownership
  // of preedit_info. If the parameter is NULL, hides the string currently
  // displayed.
  void SetPreeditInfo(Text preedit_info);
  // Displays an auxiliary message (e.g., an error message, a title of
  // candidate window). If the string is empty (""), hides the message
  // currently being displayed.
  void SetAuxString(const std::string& str);
  // Displays the typed reading above the candidate list during live
  // conversion. This state is kept separate from candidate footer text.
  // `before_live_conversion` marks a reading the server exposes before live
  // conversion has started; the preedit is still that same reading then.
  void SetLiveConversionReading(const std::string& reading,
                                bool before_live_conversion = false);
  // Displays what produced the visible preedit (Mozc, Zenz pending, Zenz).
  void SetConversionStatusLabel(const std::string& label);
  mozc::config::Config::ConversionStatusIndicatorLabel
  conversion_status_label_style() const {
    return conversion_status_label_style_;
  }
  // Sets a current composition mode (e.g., Hankaku Katakana).
  void SetCompositionMode(mozc::commands::CompositionMode mode,
                          bool updateUI = true);

  void SendCompositionMode(mozc::commands::CompositionMode mode);

  // Sets the url to be opened by the default browser.
  void SetUrl(const std::string& url);

  const std::string& GetIconFile(std::string key);

  mozc::commands::CompositionMode GetCompositionMode() {
    return composition_mode_;
  }

  MozcClientInterface* GetClient() const;
  void ReleaseClient();

  bool SendCommand(const mozc::commands::SessionCommand& session_command,
                   mozc::commands::Output* new_output);

  void SetUsage(const std::string& title, const std::string& description);

  void DrawAll();

  // Sends `command` to the server after `delay_msec` and applies the reply.
  // There is one timer per command type: a newer request of the same type
  // replaces the pending one (server generations are monotonic per type),
  // while different types never cancel each other.  The command is dropped if
  // the input context lost focus or changed its input domain in the meantime;
  // this is checked again when the timer fires (focus, focus epoch and the
  // secure-input rule), not only by cancelling on those transitions.  While
  // the usage panel is shown a due command stays pending instead of being
  // sent.
  void ScheduleDelayedSessionCommand(
      const mozc::commands::SessionCommand& command, uint32_t delay_msec);
  // Drops every pending delayed command.
  void CancelDelayedSessionCommands();

  // A composition that was not typed (reverse conversion) cannot be rebuilt
  // from the raw-key journal after a server restart.
  void MarkCompositionNotReplayable();

  // The one place that hands application text to the converter: sends
  // CONVERT_REVERSE for the application selection described by `selection`.
  // Nothing is sent in secure input or for a selection that is not
  // SelectionForReconversion::kReconvertible (too long, more than one line,
  // or not covered by the reported text).
  // On kStarted `reply` is ready to be parsed: it carries the deletion range
  // that removes the selection, immediate callbacks are stripped, the
  // composition is marked non-replayable and a pending keymap macro is
  // acknowledged.  Used by the Space fallback and by the reconvert key.
  ReverseConversionResult ReverseConvertSelection(
      const SurroundingTextInfo& selection, mozc::commands::Output* reply);

 private:
  void DisplayUsage();
  // Sends key event to the server. If the IPC succeeds, returns true and the
  // response is stored on 'out' (and 'out_error' is not modified). If the IPC
  // fails, returns false and the error message is stored on 'out_error'. In
  // this case, 'out' is not modified.
  // On success 'sent_event' is 'event' as the server received it: with the
  // composition mode of the session it was sent to, which can be a
  // replacement session found during the dispatch.
  bool TrySendKeyEvent(InputContext* ic, const mozc::commands::KeyEvent& event,
                       mozc::commands::Output* out,
                       mozc::commands::KeyEvent* sent_event,
                       std::string* out_error) const;

  // Sends 'mouse click on the candidate window' event to the server.
  bool TrySendClick(int32_t unique_id,
                    mozc::commands::SessionCommand::CommandType type,
                    mozc::commands::Output* out, std::string* out_error) const;

  // Sends composition mode to the server.
  bool TrySendCompositionMode(mozc::commands::CompositionMode mode,
                              mozc::commands::Output* out,
                              std::string* out_error) const;

  // Sends a command to the server.
  bool TrySendCommand(mozc::commands::SessionCommand::CommandType type,
                      mozc::commands::Output* out,
                      std::string* out_error) const;

  bool TrySendRawCommand(const mozc::commands::SessionCommand& command,
                         mozc::commands::Output* out,
                         std::string* out_error) const;

  mozc::commands::Context BuildContext(bool include_surrounding_text) const;
  void AdvanceFocusEpoch();
  bool InitializeSessionGeneration(
      MozcClientInterface* client, uint64_t generation,
      const mozc::commands::Context& context);
  bool ApplyInitialModeForCurrentClient();

  // Parses the response from mozc_server. Returns whether the server consumes
  // the input or not (true means 'consumed').  `source_key` is the key event
  // this response answers, or nullptr for responses to session commands,
  // delayed callbacks and composed strings.
  bool ParseResponse(mozc::commands::Output raw_response,
                     const mozc::commands::KeyEvent* source_key = nullptr);

  // Handles the RECONVERT_SELECTION_OR_INSERT_SPACE callback before anything
  // of `response` is applied.
  // - No explicit application selection (or secure input): the fallback is
  //   kept and a plain Space is handed back to the application as a key.
  // - A selection that is reconverted: `response` is replaced with the reply
  //   to the reverse conversion.
  // - A selection that is not reconverted (refused by the server, command
  //   failed, too large to send): the key is consumed and nothing is applied,
  //   so Space never replaces the selected text.
  void ResolveReconvertSelectionOrFallback(
      const mozc::commands::KeyEvent* source_key,
      mozc::commands::Output* response);

  // Applies the reply to a delayed command.  A reply that would redraw exactly
  // what is on the screen only re-arms its callback.
  void HandleDelayedCommandResponse(mozc::commands::Output response);

  // Cancels the delayed commands a key sent to the server makes stale.
  void CancelDelayedSessionCommandsInvalidatedByKey() const;

  // True while a preedit or a candidate list is shown for this context.
  bool HasVisibleComposition() const;

  void ClearAll();
  void DrawPreeditInfo();
  void DrawAux();

  // Open url_ with a default browser.
  void OpenUrl();

  InputContext* ic_;
  MozcEngine* engine_;
  mutable std::shared_ptr<MozcClientInterface> client_;
  mutable RawReadingRecovery raw_reading_recovery_;
  uint64_t focus_epoch_ = 1;
  uint64_t initial_mode_client_generation_ = 0;

  mozc::commands::CompositionMode composition_mode_ = mozc::commands::HIRAGANA;
  mozc::config::Config::PreeditMethod preedit_method_ =
      mozc::config::Config::ROMAN;
  const std::unique_ptr<KeyEventHandler> handler_;

  bool displayUsage_ = false;
  bool show_live_conversion_reading_ = true;
  bool show_conversion_status_ = true;
  mozc::config::Config::ConversionStatusIndicatorLabel
      conversion_status_label_style_ =
          mozc::config::Config::CONVERSION_STATUS_FULL_LABEL;
  Text preedit_;
  std::string aux_;  // error tooltip, or candidate window title.
  std::string live_conversion_reading_;
  bool reading_is_before_live_conversion_ = false;
  std::string conversion_status_label_;
  // DisplaySignature() of the Output drawn last by ParseResponse(), or empty
  // when the screen may differ from it.
  std::string last_display_signature_;
  std::string url_;  // URL to be opened by a browser.
  std::string description_;
  std::string title_;

  // Pending delayed session commands, keyed by SessionCommand::CommandType.
  mutable std::map<int, std::unique_ptr<fcitx::EventSourceTime>>
      delayed_commands_;
};

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX_FCITX_MOZC_H_
