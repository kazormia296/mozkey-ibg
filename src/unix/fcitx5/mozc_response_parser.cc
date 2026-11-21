// Copyright 2010-2012, Google Inc.
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

#include "unix/fcitx5/mozc_response_parser.h"

#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "base/vlog.h"
#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "unix/fcitx5/display_status.h"
#include "unix/fcitx5/i18nwrapper.h"
#include "unix/fcitx5/mozc_engine.h"
#include "unix/fcitx5/mozc_state.h"
#include "unix/fcitx5/session_callback.h"
#include "unix/fcitx5/surrounding_text_util.h"

namespace fcitx {

namespace {

std::string CreateDescriptionString(const std::string& description) {
  return " [" + description + "]";
}

class MozcCandidateWord final : public CandidateWord {
 public:
  MozcCandidateWord(int id, std::string text, MozcEngine* engine,
                    bool submit_on_select)
      : CandidateWord(Text(std::move(text))),
        id_(id),
        engine_(engine),
        submit_on_select_(submit_on_select) {}

  void select(InputContext* inputContext) const override {
    MozcState* mozc_state = engine_->mozcState(inputContext);
    mozc_state->SelectCandidate(id_, submit_on_select_);
  }

 private:
  int id_;
  MozcEngine* engine_;
  // True for entries of a passive suggestion list: clicking one commits it.
  bool submit_on_select_;
};

class MozcCandidateList final : public CandidateList,
                                public PageableCandidateList {
 public:
  MozcCandidateList(const mozc::commands::CandidateWindow& candidates,
                    InputContext* ic, MozcEngine* engine, bool use_annotation)
      : ic_(ic), engine_(engine) {
    auto* state = engine_->mozcState(ic);
    setPageable(this);
    bool index_visible = false;
    if (candidates.has_footer()) {
      const auto& footer = candidates.footer();
      index_visible = footer.has_index_visible() && footer.index_visible();
    }

    if (candidates.candidate_size() > 0) {
      if (candidates.candidate(0).index() > 0) {
        hasPrev_ = true;
      }
      if (candidates.candidate(candidates.candidate_size() - 1).index() + 1 <
          candidates.size()) {
        hasNext_ = true;
      }
    }
    const bool isVertical = *engine_->config().verticalList;
    // candidates.direction is never used, we just override it with our
    // configuration.
    layout_ = isVertical ? CandidateLayoutHint::Vertical
                         : CandidateLayoutHint::Horizontal;

    int focused_index = -1;
    cursor_ = -1;
    if (candidates.has_focused_index()) {
      focused_index = candidates.focused_index();
    }

    std::map<int32_t, std::pair<std::string, std::string>> usage_map;
    if (candidates.has_usages()) {
      const mozc::commands::InformationList& usages = candidates.usages();
      for (size_t i = 0; i < usages.information().size(); ++i) {
        const mozc::commands::Information& information = usages.information(i);
        if (!information.has_id() || !information.has_description()) {
          continue;
        }
        usage_map[information.id()].first = information.title();
        usage_map[information.id()].second = information.description();
      }
    }

    labels_.reserve(candidates.candidate_size());

    // Selecting an entry of a list nobody navigates would only focus it and
    // leave live conversion; a click there means "commit this suggestion".
    const bool submit_on_select = IsPassiveSuggestionWindow(candidates);

    for (int i = 0; i < candidates.candidate_size(); ++i) {
      const mozc::commands::CandidateWindow::Candidate& candidate =
          candidates.candidate(i);
      const uint32_t index = candidate.index();

      std::string value;
      if (use_annotation && candidate.has_annotation() &&
          candidate.annotation().has_prefix()) {
        value = candidate.annotation().prefix();
      }
      value += candidate.value();
      if (use_annotation && candidate.has_annotation() &&
          candidate.annotation().has_suffix()) {
        value += candidate.annotation().suffix();
      }
      if (use_annotation && candidate.has_annotation() &&
          candidate.annotation().has_description()) {
        // Display descriptions ([HALF][KATAKANA], [GREEK], [Black square],
        // etc).
        value += CreateDescriptionString(candidate.annotation().description());
      }

      const bool is_current =
          candidates.has_focused_index() && index == focused_index;
      if (is_current) {
        cursor_ = i;
      }
      if (use_annotation && candidate.has_information_id()) {
        auto it = usage_map.find(candidate.information_id());
        if (it != usage_map.end()) {
          if (*engine_->config().expandMode == ExpandMode::Always ||
              (*engine_->config().expandMode == ExpandMode::OnFocus &&
               is_current)) {
            if (it->second.first != candidate.value()) {
              value.append("\n").append(it->second.first);
            }
            value.append("\n").append(it->second.second);
          } else if (*engine_->config().expandMode == ExpandMode::Hotkey &&
                     is_current && engine_->config().expand->isValid()) {
            state->SetUsage(it->second.first, it->second.second);
            // We don't have a good library option for this, just do the simple
            // replace. absl's runtime parsed format string is too copmlex.
            std::string msg = _("Press %s to show usages.");
            msg = stringutils::replaceAll(msg, "%s",
                                          engine_->config().expand->toString());
            value += CreateDescriptionString(msg);
          }
        }
      }

      if (candidate.has_annotation() && candidate.annotation().has_shortcut()) {
        labels_.emplace_back(candidate.annotation().shortcut() + ". ");
      } else if (index_visible) {
        labels_.emplace_back(std::to_string(i + 1) + ". ");
      } else {
        labels_.emplace_back();
      }

      int32_t id = kBadCandidateId;
      if (candidate.has_id()) {
        id = candidate.id();
        DCHECK_NE(kBadCandidateId, id) << "Unexpected id is passed.";
      }
      candidateWords_.emplace_back(std::make_unique<MozcCandidateWord>(
          id, value, engine, submit_on_select));
    }
  }

  const Text& label(int idx) const override {
    checkIndex(idx);
    return labels_[idx];
  }

  const CandidateWord& candidate(int idx) const override {
    checkIndex(idx);
    return *candidateWords_[idx];
  }
  int size() const override { return candidateWords_.size(); }

  int cursorIndex() const override { return cursor_; }

  CandidateLayoutHint layoutHint() const override { return layout_; }

  bool hasPrev() const override { return hasPrev_; }
  bool hasNext() const override { return hasNext_; }
  void prev() override {
    auto* mozc_state = engine_->mozcState(ic_);
    mozc_state->Paging(true);
  }
  void next() override {
    auto* mozc_state = engine_->mozcState(ic_);
    mozc_state->Paging(false);
  }

  bool usedNextBefore() const override { return true; }

 private:
  void checkIndex(int idx) const {
    if (idx < 0 && idx >= size()) {
      throw std::invalid_argument("invalid index");
    }
  }

  InputContext* ic_;
  MozcEngine* engine_;
  std::vector<Text> labels_;
  bool hasPrev_ = false;
  bool hasNext_ = false;
  CandidateLayoutHint layout_ = CandidateLayoutHint::Vertical;
  int cursor_ = -1;
  std::vector<std::unique_ptr<CandidateWord>> candidateWords_;
};

}  // namespace

MozcResponseParser::MozcResponseParser(MozcEngine* engine) : engine_(engine) {}

MozcResponseParser::~MozcResponseParser() {}

void MozcResponseParser::UpdateDeletionRange(
    const mozc::commands::Output& response, InputContext* ic) const {
  if (response.has_deletion_range() &&
      response.deletion_range().offset() <= 0 &&
      response.deletion_range().offset() + response.deletion_range().length() >=
          0) {
    ic->deleteSurroundingText(response.deletion_range().offset(),
                              response.deletion_range().length());
  }
}

void MozcResponseParser::LaunchTool(const mozc::commands::Output& response,
                                    InputContext* ic) const {
  if (response.has_launch_tool_mode()) {
    auto* mozc_state = engine_->mozcState(ic);
    mozc_state->GetClient()->LaunchToolWithProtoBuf(response);
  }
}

void MozcResponseParser::ExecuteCallback(CallbackAction action,
                                         InputContext* ic) const {
  // Delayed callbacks were scheduled by ParseResponseInternal already.  Only
  // the callbacks that have to run right now, with the application text at
  // hand, are executed here.
  auto* mozc_state = engine_->mozcState(ic);
  mozc::commands::Output new_output;

  switch (action) {
    case CallbackAction::kUndo: {
      mozc::commands::SessionCommand session_command;
      session_command.set_type(mozc::commands::SessionCommand::UNDO);
      if (!mozc_state->SendCommand(session_command, &new_output)) {
        LOG(ERROR) << "Callback Command Failed";
        return;
      }
      MOZC_VLOG(1) << "New output" << new_output.DebugString();
      break;
    }
    case CallbackAction::kConvertReverse: {
      // PlanCallback never selects this action in secure input; keep the
      // guard next to the code that reads application text anyway.
      if (mozc_state->IsSecureInput()) {
        return;
      }
      // |relative_selected_length| represents how many characters are
      // selected as a relative distance of characters. Positive value
      // represents forward text selection and negative value represents
      // backword text selection.
      SurroundingTextInfo surrounding_text_info;
      if (!GetSurroundingText(ic, &surrounding_text_info,
                              engine_->clipboardAddon())) {
        return;
      }
      // The same rules as for the Space fallback, enforced in one place:
      // only a short single-line selection that the reported text really
      // covers is sent, the reply is accepted only from the session that was
      // asked, and the selection is removed only when a reconversion really
      // started.  The reply is not logged; it echoes application text.
      if (mozc_state->ReverseConvertSelection(surrounding_text_info,
                                              &new_output) ==
          MozcState::ReverseConversionResult::kNotSent) {
        return;
      }
      // A refusal is still parsed below: it carries no deletion range and
      // only brings the panel in line with what the server answered.
      break;
    }
    default:
      return;
  }

  // The reply may still schedule a delayed callback (Undo re-arms live
  // conversion), but an immediate callback inside it is not followed.
  ParseResponseInternal(new_output, ic, /*allow_immediate_callback=*/false);
}

bool MozcResponseParser::ParseResponse(const mozc::commands::Output& response,
                                       InputContext* ic) const {
  return ParseResponseInternal(response, ic,
                               /*allow_immediate_callback=*/true);
}

bool MozcResponseParser::ParseResponseInternal(
    const mozc::commands::Output& response, InputContext* ic,
    bool allow_immediate_callback) const {
  auto* mozc_state = engine_->mozcState(ic);
  mozc_state->SetUsage("", "");

  UpdateDeletionRange(response, ic);

  // We should check the mode field first since the response for a
  // SWITCH_COMPOSITION_MODE request only contains mode and id fields.
  if (response.has_mode()) {
    mozc_state->SetCompositionMode(
        response.mode(), !engine_->deactivating() &&
                             engine_->instance()->inputMethod(ic) ==
                                 "mozkey-ibg");
  }

  // A delayed callback is independent of what the Output does with the key:
  // the continuation of a keymap macro arrives on an Output that keeps the
  // original key's "not consumed" answer.  Only the callback is delayed; the
  // Output itself is applied below as usual.
  const CallbackPlan callback_plan =
      PlanCallback(response, mozc_state->IsSecureInput());
  if (callback_plan.action == CallbackAction::kSchedule) {
    mozc_state->ScheduleDelayedSessionCommand(callback_plan.command,
                                              callback_plan.delay_msec);
  }

  if (!response.consumed()) {
    // The key was not consumed by Mozc.
    return false;
  }

  if (response.has_result()) {
    const mozc::commands::Result& result = response.result();
    ParseResult(result, ic);
  }

  mozc_state->SetLiveConversionReading(
      BuildDisplayReading(response),
      /*before_live_conversion=*/!response.live_conversion());
  mozc_state->SetConversionStatusLabel(ConversionStatusLabel(
      response, mozc_state->conversion_status_label_style()));

  // First, determine the cursor position.
  if (response.has_preedit()) {
    const mozc::commands::Preedit& preedit = response.preedit();
    ParsePreedit(preedit, PreeditCaretPosition(response), ic);
  }

  // Then show the candidate window.
  if (response.has_candidate_window()) {
    const mozc::commands::CandidateWindow& candidates =
        response.candidate_window();
    ParseCandidates(candidates, ic);
  }

  if (response.has_url()) {
    const std::string& url = response.url();
    mozc_state->SetUrl(url);
  }
  LaunchTool(response, ic);
  if (allow_immediate_callback) {
    ExecuteCallback(callback_plan.action, ic);
  }

  return true;  // mozc consumed the key.
}

void MozcResponseParser::ParseResult(const mozc::commands::Result& result,
                                     InputContext* ic) const {
  auto* mozc_state = engine_->mozcState(ic);
  switch (result.type()) {
    case mozc::commands::Result::NONE: {
      mozc_state->SetAuxString("No result");  // not a fatal error.
      break;
    }
    case mozc::commands::Result::STRING: {
      mozc_state->SetResultString(result.value());
      break;
    }
  }
}

void MozcResponseParser::ParseCandidates(
    const mozc::commands::CandidateWindow& candidates, InputContext* ic) const {
  auto* mozc_state = engine_->mozcState(ic);
  const mozc::commands::Footer& footer = candidates.footer();
  if (candidates.has_footer()) {
    std::string auxString;
    if (footer.has_label()) {
      // TODO(yusukes,mozc-team): label() is not localized. Currently, it's
      // always
      // written in Japanese (in UTF-8).
      auxString += footer.label();
    } else if (footer.has_sub_label()) {
      // Windows client shows sub_label() only when label() is not specified. We
      // follow the policy.
      auxString += footer.sub_label();
    }

    if (footer.has_index_visible() && footer.index_visible()) {
      if (!auxString.empty()) {
        auxString += " ";
      }
      auxString += std::to_string(candidates.focused_index() + 1);
      auxString += "/";
      auxString += std::to_string(candidates.size());
    }
    mozc_state->SetAuxString(auxString);
  }

  ic->inputPanel().setCandidateList(std::make_unique<MozcCandidateList>(
      candidates, ic, engine_, *engine_->config().verticalList));
}

void MozcResponseParser::ParsePreedit(const mozc::commands::Preedit& preedit,
                                      uint32_t position,
                                      InputContext* ic) const {
  auto* mozc_state = engine_->mozcState(ic);
  Text preedit_text;
  std::string s;

  for (int i = 0; i < preedit.segment_size(); ++i) {
    const mozc::commands::Preedit_Segment& segment = preedit.segment(i);
    const std::string& str = segment.value();
    if (!utf8::validate(str)) {
      continue;
    }
    TextFormatFlags format_flag;

    switch (segment.annotation()) {
      case mozc::commands::Preedit_Segment::NONE:
        break;
      case mozc::commands::Preedit_Segment::UNDERLINE:
        format_flag = TextFormatFlag::Underline;
        break;
      case mozc::commands::Preedit_Segment::HIGHLIGHT:
        format_flag = TextFormatFlag::HighLight;
        break;
    }
    s += str;

    preedit_text.append(str, format_flag);
  }

  int cursor = -1;
  auto charLength = utf8::length(s);
  if (charLength >= position) {
    cursor = utf8::ncharByteLength(s.begin(), position);
  }
  preedit_text.setCursor(cursor);

  mozc_state->SetPreeditInfo(std::move(preedit_text));
}

}  // namespace fcitx
