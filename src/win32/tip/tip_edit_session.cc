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

#include "win32/tip/tip_edit_session.h"

#include <msctf.h>
#include <wil/com.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "base/win32/com.h"
#include "base/win32/wide_char.h"
#include "client/client_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/renderer_callback_provenance.h"
#include "win32/base/conversion_mode_util.h"
#include "win32/base/deleter.h"
#include "win32/base/input_state.h"
#include "win32/tip/tip_composition_util.h"
#include "win32/tip/tip_dll_module.h"
#include "win32/tip/tip_edit_session_impl.h"
#include "win32/tip/tip_grimodex_context_util.h"
#include "win32/tip/tip_input_mode_manager.h"
#include "win32/tip/tip_private_context.h"
#include "win32/tip/tip_range_util.h"
#include "win32/tip/tip_status.h"
#include "win32/tip/tip_surrounding_text.h"
#include "win32/tip/tip_text_service.h"
#include "win32/tip/tip_thread_context.h"
#include "win32/tip/tip_ui_handler.h"

namespace mozc {
namespace win32 {
namespace tsf {

std::optional<std::size_t> GetValidPrecedingDeletionLength(
    const commands::DeletionRange& deletion_range) {
  const int64_t offset = static_cast<int64_t>(deletion_range.offset());
  const int64_t length = static_cast<int64_t>(deletion_range.length());
  if (offset >= 0 || length <= 0 || -offset != length ||
      length > static_cast<int64_t>(kMaxTsfDeletionLength)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(length);
}

namespace {

using ::mozc::commands::DeletionRange;
using ::mozc::commands::Output;
using ::mozc::commands::SessionCommand;
using CompositionMode = ::mozc::commands::CompositionMode;
using SpecialKey = ::mozc::commands::KeyEvent_SpecialKey;
using CommandType = ::mozc::commands::SessionCommand::CommandType;

bool IsContextFocusedForSnapshot(TipTextService* text_service,
                                 ITfContext* expected_context,
                                 TsfFocusSnapshot snapshot,
                                 bool require_nonsecure) {
  return IsTsfContextFocusedForSnapshot(text_service, expected_context,
                                        snapshot, require_nonsecure);
}

bool SendConvertReverseForFocus(
    TipTextService* text_service, ITfContext* context,
    client::ClientInterface* client, std::string_view selected_text,
    TsfFocusSnapshot snapshot, Output* output,
    uint64_t* output_application_generation) {
  if (context == nullptr || client == nullptr || output == nullptr ||
      selected_text.empty() || output_application_generation == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    return false;
  }
  if (!IsContextFocusedForSnapshot(text_service, context, snapshot,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  if (*output_application_generation == 0) {
    *output_application_generation =
        private_context->ReserveOutputApplicationForFocusDomain(
            snapshot.focus_epoch, snapshot.focus_revision);
  }
  const auto is_output_application_current = [&]() {
    return private_context->IsOutputApplicationForFocusDomain(
        snapshot.focus_epoch, snapshot.focus_revision,
        *output_application_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context, snapshot,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }

  // Context construction resolves the attached application and can reenter
  // TSF.  Do not place application text on the command wire until that call is
  // complete and the domain token is still current and non-secure.
  commands::Context client_context = BuildTsfMozcContext(
      text_service, context, /*include_surrounding_text=*/false);
  if (!IsContextFocusedForSnapshot(text_service, context, snapshot,
                                   /*require_nonsecure=*/true) ||
      !client_context.has_grimodex() ||
      client_context.grimodex().secure_input() ||
      !is_output_application_current()) {
    return false;
  }

  SessionCommand command;
  command.set_type(SessionCommand::CONVERT_REVERSE);
  command.set_text(std::string(selected_text));
  if (!IsContextFocusedForSnapshot(text_service, context, snapshot,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current() ||
      !client->SendCommandWithContext(command, client_context, output)) {
    return false;
  }
  return IsContextFocusedForSnapshot(text_service, context, snapshot,
                                     /*require_nonsecure=*/true) &&
         is_output_application_current();
}

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class AsyncLayoutChangeEditSessionImpl final
    : public TipComImplements<ITfEditSession> {
 public:
  AsyncLayoutChangeEditSessionImpl(
      wil::com_ptr_nothrow<TipTextService> text_service,
      wil::com_ptr_nothrow<ITfContext> context,
      TsfFocusSnapshot layout_domain,
      uint64_t mode_callback_generation,
      TipModeCallbackKind mode_callback_kind)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        layout_domain_(layout_domain),
        mode_callback_generation_(mode_callback_generation),
        mode_callback_kind_(mode_callback_kind) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie read_cookie) override {
    const auto is_layout_transaction_current = [&]() {
      return IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                         layout_domain_,
                                         /*require_nonsecure=*/true) &&
             (mode_callback_generation_ == 0 ||
              text_service_->IsModeCallbackCurrent(
                  mode_callback_kind_, mode_callback_generation_));
    };
    if (!is_layout_transaction_current()) {
      return S_OK;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        text_service_->GetThreadContextLease();
    if (!thread_context) {
      return S_OK;
    }
    // Ignore the returned code as TipUiHandler::UpdateUI will be called
    // anyway.
    thread_context->GetInputModeManager()->OnMoveFocusedWindow();
    if (!is_layout_transaction_current()) {
      return S_OK;
    }

    TipEditSessionImpl::UpdateUI(
        text_service_.get(), context_.get(), read_cookie,
        layout_domain_.focus_epoch, layout_domain_.focus_revision);
    if (!is_layout_transaction_current()) {
      // A nested newer compartment callback can render before this outer
      // UpdateUI returns.  Queue one current-state pass so stale UI cannot be
      // the final visible state.
      text_service_->PostUIUpdateMessage();
    }
    return S_OK;
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  TsfFocusSnapshot layout_domain_;
  uint64_t mode_callback_generation_ = 0;
  TipModeCallbackKind mode_callback_kind_ =
      TipModeCallbackKind::kConversion;
};

bool OnLayoutChangedAsyncImpl(TipTextService* text_service,
                              ITfContext* context,
                              TsfFocusSnapshot layout_domain,
                              uint64_t mode_callback_generation = 0,
                              TipModeCallbackKind mode_callback_kind =
                                  TipModeCallbackKind::kConversion) {
  if (!text_service || !context) {
    return false;
  }

  const auto is_layout_transaction_current = [&]() {
    return IsContextFocusedForSnapshot(text_service, context, layout_domain,
                                       /*require_nonsecure=*/true) &&
           (mode_callback_generation == 0 ||
            text_service->IsModeCallbackCurrent(mode_callback_kind,
                                                mode_callback_generation));
  };
  if (!is_layout_transaction_current()) {
    return false;
  }
  auto edit_session = MakeComPtr<AsyncLayoutChangeEditSessionImpl>(
      text_service, context, layout_domain, mode_callback_generation,
      mode_callback_kind);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = context->RequestEditSession(
      text_service->GetClientID(), edit_session.get(),
      TF_ES_ASYNCDONTCARE | TF_ES_READ, &edit_session_result);
  return SUCCEEDED(hr) && SUCCEEDED(edit_session_result) &&
         is_layout_transaction_current();
}

bool OnLayoutChangedAsyncImpl(TipTextService* text_service,
                              ITfContext* context) {
  if (text_service == nullptr) {
    return false;
  }
  return OnLayoutChangedAsyncImpl(
      text_service, context,
      CaptureTsfFocusSnapshot(text_service->GetThreadContext()));
}

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class AsyncSetFocusEditSessionImpl final
    : public TipComImplements<ITfEditSession> {
 public:
  AsyncSetFocusEditSessionImpl(
      wil::com_ptr_nothrow<TipTextService> text_service,
      wil::com_ptr_nothrow<ITfContext> context, uint64_t scheduled_focus_epoch,
      int32_t scheduled_focus_revision)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        scheduled_focus_epoch_(scheduled_focus_epoch),
        scheduled_focus_revision_(scheduled_focus_revision) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie read_cookie) override {
    const TsfFocusSnapshot scheduled_domain = {
        .focus_epoch = scheduled_focus_epoch_,
        .focus_revision = scheduled_focus_revision_,
    };
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/false)) {
      return S_OK;
    }
    std::vector<InputScope> input_scopes;
    bool input_scopes_resolved = false;
    wil::com_ptr_nothrow<ITfRange> selection_range;
    TfActiveSelEnd active_sel_end = TF_AE_NONE;
    if (SUCCEEDED(TipRangeUtil::GetDefaultSelection(
            context_.get(), read_cookie, &selection_range, &active_sel_end))) {
      input_scopes_resolved = SUCCEEDED(TipRangeUtil::GetInputScopes(
          selection_range.get(), read_cookie, &input_scopes));
    }
    if (!input_scopes_resolved) {
      // If TSF withholds the input scope, do not risk selecting project data
      // for a password field. A later authoritative scope update can leave
      // secure mode with a newer epoch.
      input_scopes.push_back(IS_PASSWORD);
    }
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/false)) {
      return S_OK;
    }
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager =
        text_service_->GetThreadManager();
    const std::shared_ptr<TipThreadContext> thread_context =
        text_service_->GetThreadContextLease();
    if (!thread_manager || !thread_context) {
      return S_OK;
    }
    DWORD system_input_mode = 0;
    if (!TipStatus::GetInputModeConversion(
            thread_manager.get(), text_service_->GetClientID(),
            &system_input_mode)) {
      return E_FAIL;
    }
    const bool system_open = TipStatus::IsOpen(thread_manager.get());
    const std::string program = GetAttachedProgram(context_.get());
    // The TSF calls above can cross a component boundary.  Recheck immediately
    // before replacing the synchronous fail-closed scope.
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/false)) {
      return S_OK;
    }
    const auto action = thread_context->GetInputModeManager()->OnSetFocus(
        system_open, system_input_mode, input_scopes);
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/false)) {
      return S_OK;
    }
    const std::shared_ptr<TipPrivateContext> private_context =
        text_service_->GetPrivateContext(context_.get());
    if (private_context == nullptr) {
      return S_OK;
    }
    TsfFocusSnapshot resolved_domain;
    if (!SendTsfResetContextForProgram(
            text_service_.get(), program, private_context->GetClient(),
            thread_context->GetInputModeManager()->IsPasswordInputScope(),
            &resolved_domain)) {
      return E_FAIL;
    }
    // RESET_CONTEXT installs the resolved application/security domain and can
    // advance the epoch.  Never render retained output from the temporary
    // fail-closed domain used while InputScope was unresolved.
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     resolved_domain,
                                     /*require_nonsecure=*/false)) {
      return S_OK;
    }
    if (action == TipInputModeManager::kUpdateUI) {
      TipEditSessionImpl::UpdateUI(text_service_.get(), context_.get(),
                                   read_cookie, resolved_domain.focus_epoch,
                                   resolved_domain.focus_revision);
    }
    return S_OK;
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  uint64_t scheduled_focus_epoch_;
  int32_t scheduled_focus_revision_;
};

bool OnUpdateOnOffModeAsync(TipTextService* text_service, ITfContext* context,
                            bool open, TsfFocusSnapshot mode_domain,
                            uint64_t mode_callback_generation) {
  const auto is_mode_transaction_current = [&]() {
    return IsContextFocusedForSnapshot(text_service, context, mode_domain,
                                       /*require_nonsecure=*/false) &&
           (mode_callback_generation == 0 ||
            text_service->IsModeCallbackCurrent(
                TipModeCallbackKind::kOpenClose,
                mode_callback_generation));
  };
  if (!is_mode_transaction_current()) {
    return false;
  }
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context) {
    return false;
  }
  const auto action =
      thread_context->GetInputModeManager()->OnChangeOpenClose(open);
  if (!is_mode_transaction_current()) {
    return false;
  }
  if (action == TipInputModeManager::kUpdateUI) {
    return OnLayoutChangedAsyncImpl(text_service, context, mode_domain,
                                    mode_callback_generation,
                                    TipModeCallbackKind::kOpenClose);
  }
  return true;
}

bool OnUpdateOnOffModeAsync(TipTextService* text_service, ITfContext* context,
                            bool open) {
  if (text_service == nullptr) {
    return false;
  }
  return OnUpdateOnOffModeAsync(
      text_service, context, open,
      CaptureTsfFocusSnapshot(text_service->GetThreadContext()),
      /*mode_callback_generation=*/0);
}

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class AsyncSwitchInputModeEditSessionImpl final
    : public TipComImplements<ITfEditSession> {
 public:
  AsyncSwitchInputModeEditSessionImpl(
      wil::com_ptr_nothrow<TipTextService> text_service,
      wil::com_ptr_nothrow<ITfContext> context, bool open, uint32_t native_mode,
      commands::Context client_context, uint64_t scheduled_focus_epoch,
      int32_t scheduled_focus_revision,
      uint64_t output_application_generation)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        open_(open),
        native_mode_(native_mode),
        client_context_(std::move(client_context)),
        scheduled_focus_epoch_(scheduled_focus_epoch),
        scheduled_focus_revision_(scheduled_focus_revision),
        output_application_generation_(output_application_generation) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie write_cookie) override {
    const TsfFocusSnapshot scheduled_domain = {
        .focus_epoch = scheduled_focus_epoch_,
        .focus_revision = scheduled_focus_revision_,
    };
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true)) {
      return S_OK;
    }
    const std::shared_ptr<TipPrivateContext> private_context =
        text_service_->GetPrivateContext(context_.get());
    if (!private_context) {
      // This is an unmanaged context. It's OK. Nothing to do.
      return S_OK;
    }

    const std::shared_ptr<TipThreadContext> thread_context =
        text_service_->GetThreadContextLease();
    if (!thread_context) {
      return S_OK;
    }
    const TipInputModeManager* input_mode_manager =
        thread_context->GetInputModeManager();

    CompositionMode mozc_mode = commands::HIRAGANA;
    if (!ConversionModeUtil::ToMozcMode(native_mode_, &mozc_mode)) {
      return E_FAIL;
    }
    Output output;
    const auto is_output_application_current = [&]() {
      return private_context->IsOutputApplicationForFocusDomain(
          scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
          output_application_generation_);
    };
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true) ||
        !is_output_application_current()) {
      return S_OK;
    }
    if (!open_) {
      // The next on/off mode is OFF. Send TURN_OFF_IME to update the converter
      // state.
      SessionCommand command;
      command.set_type(commands::SessionCommand::TURN_OFF_IME);
      command.set_composition_mode(mozc_mode);
      if (!private_context->GetClient()->SendCommandWithContext(
              command, client_context_, &output)) {
        return E_FAIL;
      }
    } else if (!input_mode_manager->GetEffectiveOpenClose()) {
      // The next on/off mode is ON but the state of input mode manager is
      // OFF. Send TURN_ON_IME to update the converter state.
      SessionCommand command;
      command.set_type(commands::SessionCommand::TURN_ON_IME);
      command.set_composition_mode(mozc_mode);
      if (!private_context->GetClient()->SendCommandWithContext(
              command, client_context_, &output)) {
        return E_FAIL;
      }
    } else {
      // The next on/off mode and the state of input mode manager is
      // consistent. Send SWITCH_COMPOSITION_MODE to update the converter state.
      SessionCommand command;
      command.set_type(SessionCommand::SWITCH_COMPOSITION_MODE);
      command.set_composition_mode(mozc_mode);
      if (!private_context->GetClient()->SendCommandWithContext(
              command, client_context_, &output)) {
        return E_FAIL;
      }
    }
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true) ||
        !is_output_application_current()) {
      return S_OK;
    }
    return TipEditSessionImpl::UpdateContext(
        text_service_.get(), context_.get(), write_cookie, output,
        scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
        output_application_generation_);
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  bool open_;
  uint32_t native_mode_;
  commands::Context client_context_;
  uint64_t scheduled_focus_epoch_;
  int32_t scheduled_focus_revision_;
  uint64_t output_application_generation_;
};

bool OnSwitchInputModeAsync(TipTextService* text_service, ITfContext* context,
                            bool open, uint32_t native_mode,
                            TsfFocusSnapshot scheduled_domain,
                            uint64_t output_application_generation) {
  if (text_service == nullptr || context == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    return false;
  }
  const auto is_output_application_current = [&]() {
    return private_context->IsOutputApplicationForFocusDomain(
        scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
        output_application_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context, scheduled_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }
  commands::Context client_context = BuildTsfMozcContext(
      text_service, context, /*include_surrounding_text=*/false);
  if (!IsContextFocusedForSnapshot(text_service, context, scheduled_domain,
                                   /*require_nonsecure=*/true) ||
      !client_context.has_grimodex() ||
      client_context.grimodex().secure_input() ||
      !is_output_application_current()) {
    return false;
  }
  auto edit_session = MakeComPtr<AsyncSwitchInputModeEditSessionImpl>(
      text_service, context, open, native_mode, std::move(client_context),
      scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
      output_application_generation);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = context->RequestEditSession(
      text_service->GetClientID(), edit_session.get(),
      TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &edit_session_result);
  return SUCCEEDED(hr) && SUCCEEDED(edit_session_result) &&
         is_output_application_current();
}

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class AsyncSessionCommandEditSessionImpl final
    : public TipComImplements<ITfEditSession> {
 public:
  AsyncSessionCommandEditSessionImpl(
      wil::com_ptr_nothrow<TipTextService> text_service,
      wil::com_ptr_nothrow<ITfContext> context,
      const SessionCommand& session_command, commands::Context client_context,
      uint64_t scheduled_focus_epoch, int32_t scheduled_focus_revision,
      uint64_t expected_output_generation,
      uint64_t expected_composition_generation,
      uint64_t output_application_generation)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        session_command_(session_command),
        client_context_(std::move(client_context)),
        scheduled_focus_epoch_(scheduled_focus_epoch),
        scheduled_focus_revision_(scheduled_focus_revision),
        expected_output_generation_(expected_output_generation),
        expected_composition_generation_(expected_composition_generation),
        output_application_generation_(output_application_generation) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie write_cookie) override {
    const TsfFocusSnapshot scheduled_domain = {
        .focus_epoch = scheduled_focus_epoch_,
        .focus_revision = scheduled_focus_revision_,
    };
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true)) {
      return S_OK;
    }
    Output output;
    const std::shared_ptr<TipPrivateContext> private_context =
        text_service_->GetPrivateContext(context_.get());
    if (private_context == nullptr) {
      return E_FAIL;
    }
    const auto is_expected_generation_current = [&]() {
      return expected_output_generation_ == 0 ||
             private_context->IsLastOutputForFocusDomainAndGeneration(
                 scheduled_domain.focus_epoch,
                 scheduled_domain.focus_revision,
                 expected_output_generation_);
    };
    const auto is_expected_composition_current = [&]() {
      return expected_composition_generation_ == 0 ||
             private_context->IsCompositionForFocusDomainAndGeneration(
                 scheduled_domain.focus_epoch,
                 scheduled_domain.focus_revision,
                 expected_composition_generation_);
    };
    if (!is_expected_generation_current() ||
        !is_expected_composition_current()) {
      return S_OK;
    }
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true)) {
      return S_OK;
    }
    if (!is_expected_generation_current() ||
        !is_expected_composition_current()) {
      return S_OK;
    }
    const auto is_output_application_current = [&]() {
      return private_context->IsOutputApplicationForFocusDomain(
          scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
          output_application_generation_);
    };
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true) ||
        !is_expected_generation_current() ||
        !is_expected_composition_current() ||
        !is_output_application_current()) {
      return S_OK;
    }
    if (!private_context->GetClient()->SendCommandWithContext(
            session_command_, client_context_, &output)) {
      return E_FAIL;
    }
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true) ||
        !is_expected_generation_current() ||
        !is_expected_composition_current() ||
        !is_output_application_current()) {
      return S_OK;
    }

    // A delayed session command may produce another delayed callback.
    // Example:
    //   APPLY_LIVE_CONVERSION -> output callback APPLY_ZENZ_LIVE_CORRECTION
    //
    // The normal key-event path handles this in OnOutputReceivedImpl(), but
    // this edit session is used by timer-fired callbacks and bypasses that
    // function.  Therefore delayed callback chaining must be handled here too.
    if (output.has_callback() &&
        output.callback().has_session_command() &&
        output.callback().session_command().has_type()) {
      const Output::Callback& callback = output.callback();
      if (callback.has_delay_millisec() && callback.delay_millisec() > 0) {
        if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                         scheduled_domain,
                                         /*require_nonsecure=*/true) ||
            !is_expected_generation_current() ||
            !is_expected_composition_current() ||
            !is_output_application_current()) {
          return S_OK;
        }
        text_service_->PostDelayedSessionCommand(
            context_.get(),
            callback.session_command(),
            callback.delay_millisec(), output_application_generation_);
        if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                         scheduled_domain,
                                         /*require_nonsecure=*/true) ||
            !is_expected_generation_current() ||
            !is_expected_composition_current() ||
            !is_output_application_current()) {
          return S_OK;
        }

        // Apply the current output immediately.  Only the callback is delayed.
        output.clear_callback();
      }
    }

    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     scheduled_domain,
                                     /*require_nonsecure=*/true) ||
        !is_expected_generation_current() ||
        !is_expected_composition_current() ||
        !is_output_application_current()) {
      return S_OK;
    }
    return TipEditSessionImpl::UpdateContext(
        text_service_.get(), context_.get(), write_cookie, output,
        scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
        output_application_generation_);
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  SessionCommand session_command_;
  commands::Context client_context_;
  uint64_t scheduled_focus_epoch_;
  int32_t scheduled_focus_revision_;
  uint64_t expected_output_generation_;
  uint64_t expected_composition_generation_;
  uint64_t output_application_generation_;
};

bool OnSessionCommandAsync(TipTextService* text_service, ITfContext* context,
                           const SessionCommand& session_command,
                           TsfFocusSnapshot scheduled_domain,
                           uint64_t expected_output_generation,
                           uint64_t expected_output_application_generation,
                           uint64_t expected_composition_generation) {
  if (text_service == nullptr || context == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context ||
      (expected_output_generation != 0 &&
       !private_context->IsLastOutputForFocusDomainAndGeneration(
           scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
           expected_output_generation)) ||
      (expected_output_application_generation != 0 &&
       !private_context->IsOutputApplicationForFocusDomain(
           scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
           expected_output_application_generation)) ||
      (expected_composition_generation != 0 &&
       !private_context->IsCompositionForFocusDomainAndGeneration(
           scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
           expected_composition_generation))) {
    return false;
  }
  if (!IsContextFocusedForSnapshot(text_service, context, scheduled_domain,
                                   /*require_nonsecure=*/true) ||
      (expected_output_generation != 0 &&
       !private_context->IsLastOutputForFocusDomainAndGeneration(
           scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
           expected_output_generation)) ||
      (expected_output_application_generation != 0 &&
       !private_context->IsOutputApplicationForFocusDomain(
           scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
           expected_output_application_generation)) ||
      (expected_composition_generation != 0 &&
       !private_context->IsCompositionForFocusDomainAndGeneration(
           scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
           expected_composition_generation))) {
    return false;
  }
  const uint64_t output_application_generation =
      private_context->ReserveOutputApplicationForFocusDomain(
          scheduled_domain.focus_epoch, scheduled_domain.focus_revision);
  const auto is_output_application_current = [&]() {
    return private_context->IsOutputApplicationForFocusDomain(
        scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
        output_application_generation);
  };
  const auto is_bound_output_current = [&]() {
    return expected_output_generation == 0 ||
           private_context->IsLastOutputForFocusDomainAndGeneration(
               scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
               expected_output_generation);
  };
  const auto is_bound_composition_current = [&]() {
    return expected_composition_generation == 0 ||
           private_context->IsCompositionForFocusDomainAndGeneration(
               scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
               expected_composition_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context, scheduled_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current() || !is_bound_output_current() ||
      !is_bound_composition_current()) {
    return false;
  }
  commands::Context client_context = BuildTsfMozcContext(
      text_service, context, /*include_surrounding_text=*/false);
  if (!IsContextFocusedForSnapshot(text_service, context, scheduled_domain,
                                   /*require_nonsecure=*/true) ||
      !client_context.has_grimodex() ||
      client_context.grimodex().secure_input() ||
      !is_output_application_current() || !is_bound_output_current() ||
      !is_bound_composition_current()) {
    return false;
  }
  auto edit_session = MakeComPtr<AsyncSessionCommandEditSessionImpl>(
      text_service, context, session_command, std::move(client_context),
      scheduled_domain.focus_epoch, scheduled_domain.focus_revision,
      expected_output_generation, expected_composition_generation,
      output_application_generation);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = context->RequestEditSession(
      text_service->GetClientID(), edit_session.get(),
      TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &edit_session_result);
  return SUCCEEDED(hr) && SUCCEEDED(edit_session_result) &&
         is_output_application_current();
}

bool OnSessionCommandAsync(TipTextService* text_service, ITfContext* context,
                           const SessionCommand& session_command) {
  if (text_service == nullptr) {
    return false;
  }
  return OnSessionCommandAsync(
      text_service, context, session_command,
      CaptureTsfFocusSnapshot(text_service->GetThreadContext()),
      /*expected_output_generation=*/0,
      /*expected_output_application_generation=*/0,
      /*expected_composition_generation=*/0);
}

bool TurnOnImeForReconversionFallback(TipTextService* text_service,
                                      ITfContext* context,
                                      TsfFocusSnapshot output_domain,
                                      uint64_t output_application_generation) {
  if (context == nullptr) {
    return false;
  }

  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context) {
    return false;
  }
  const bool open =
      thread_context->GetInputModeManager()->GetEffectiveOpenClose();
  if (open) {
    return true;
  }

  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    // This is an unmanaged context. Keep the historical local/UI update
    // behavior, because there is no Mozc session to synchronize.
    return OnUpdateOnOffModeAsync(text_service, context, true);
  }

  if (!private_context->IsOutputApplicationForFocusDomain(
          output_domain.focus_epoch, output_domain.focus_revision,
          output_application_generation)) {
    return false;
  }
  Output output;
  SessionCommand command;
  command.set_type(SessionCommand::TURN_ON_IME);
  if (!SendTsfCommand(text_service, context, private_context->GetClient(),
                      command, &output, output_domain,
                      /*require_nonsecure=*/true,
                      &output_application_generation) ||
      !IsContextFocusedForSnapshot(text_service, context, output_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  return TipEditSession::OnOutputReceivedSync(text_service, context,
                                              std::move(output),
                                              output_domain.focus_epoch,
                                              output_domain.focus_revision,
                                              output_application_generation);
}

bool TurnOnImeAndTryToReconvertFromIme(TipTextService* text_service,
                                       ITfContext* context,
                                       TsfFocusSnapshot reconversion_domain,
                                       uint64_t output_application_generation) {
  if (context == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  const auto is_output_application_current = [&]() {
    return private_context &&
           private_context->IsOutputApplicationForFocusDomain(
               reconversion_domain.focus_epoch,
               reconversion_domain.focus_revision,
               output_application_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context, reconversion_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }

  TipSurroundingTextInfo info;
  bool need_async_edit_session = false;
  const bool prepared = TipSurroundingText::PrepareForReconversionFromIme(
      text_service, context, &info, &need_async_edit_session,
      reconversion_domain.focus_epoch, reconversion_domain.focus_revision);
  if (!IsContextFocusedForSnapshot(text_service, context, reconversion_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }
  if (!prepared) {
    // Some TSF clients do not expose enough surrounding text for reconversion.
    // In direct mode, Henkan should still behave as an IME-on key rather than
    // doing nothing.
    return TurnOnImeForReconversionFallback(
        text_service, context, reconversion_domain,
        output_application_generation);
  }

  // Currently this is not supported.
  if (info.in_composition) {
    return false;
  }

  std::string text_utf8 = WideToUtf8(info.selected_text);
  if (text_utf8.empty()) {
    // When reconversion is requested without selected text in direct mode,
    // behave as an IME-on key. This must go through the normal session path so
    // the Mozc server state and TSF open/close compartment stay synchronized.
    return TurnOnImeForReconversionFallback(
        text_service, context, reconversion_domain,
        output_application_generation);
  }

  if (!private_context) {
    // This is an unmanaged context. It's OK. Nothing to do.
    return true;
  }

  Output output;
  if (!SendConvertReverseForFocus(
          text_service, context, private_context->GetClient(), text_utf8,
          reconversion_domain, &output,
          &output_application_generation)) {
    return false;
  }

  if (output.has_callback() && output.callback().has_session_command() &&
      output.callback().session_command().has_type()) {
    // do not allow recursive call.
    return false;
  }

  if (need_async_edit_session) {
    return TipEditSession::OnOutputReceivedAsync(text_service, context,
                                                 std::move(output),
                                                 reconversion_domain.focus_epoch,
                                                 reconversion_domain.focus_revision,
                                                 output_application_generation);
  } else {
    return TipEditSession::OnOutputReceivedSync(text_service, context,
                                                std::move(output),
                                                reconversion_domain.focus_epoch,
                                                reconversion_domain.focus_revision,
                                                output_application_generation);
  }
}

bool ReconvertSelectionOrKeepFallbackOutput(TipTextService* text_service,
                                            ITfContext* context,
                                            Output* fallback_output,
                                            TsfFocusSnapshot reconversion_domain,
                                            uint64_t output_application_generation) {
  if (context == nullptr || fallback_output == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  const auto is_output_application_current = [&]() {
    return private_context &&
           private_context->IsOutputApplicationForFocusDomain(
               reconversion_domain.focus_epoch,
               reconversion_domain.focus_revision,
               output_application_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context, reconversion_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return true;
  }

  TipSurroundingTextInfo info;
  bool need_async_edit_session = false;
  const bool prepared = TipSurroundingText::PrepareForReconversionFromIme(
      text_service, context, &info, &need_async_edit_session,
      reconversion_domain.focus_epoch, reconversion_domain.focus_revision);
  if (!IsContextFocusedForSnapshot(text_service, context, reconversion_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    // Consume the old-domain callback without applying either reconversion or
    // its fallback output to the newly focused field.
    return true;
  }
  if (!prepared) {
    // Some TSF clients do not expose enough surrounding text for reconversion.
    // In that case, reconversion is unavailable, so keep the server-generated
    // fallback InsertSpace output instead of consuming Space and doing nothing.
    fallback_output->clear_callback();
    return false;
  }

  if (info.in_composition) {
    // This command is only intended for the precomposition state.  Be
    // conservative if a composition is unexpectedly found.
    return true;
  }

  const std::string text_utf8 = WideToUtf8(info.selected_text);
  if (text_utf8.empty()) {
    // No selected application text.  Keep and apply the fallback InsertSpace
    // output that the server already generated.
    fallback_output->clear_callback();
    return false;
  }

  if (!private_context) {
    // This is an unmanaged context.  Consume the key rather than applying the
    // fallback and potentially replacing selected application text.
    return true;
  }

  Output output;
  if (!SendConvertReverseForFocus(
          text_service, context, private_context->GetClient(), text_utf8,
          reconversion_domain, &output,
          &output_application_generation)) {
    return true;
  }

  if (output.has_callback() && output.callback().has_session_command() &&
      output.callback().session_command().has_type()) {
    // Do not allow recursive callbacks.
    return true;
  }

  if (need_async_edit_session) {
    return TipEditSession::OnOutputReceivedAsync(text_service, context,
                                                 std::move(output),
                                                 reconversion_domain.focus_epoch,
                                                 reconversion_domain.focus_revision,
                                                 output_application_generation);
  }
  return TipEditSession::OnOutputReceivedSync(text_service, context,
                                              std::move(output),
                                              reconversion_domain.focus_epoch,
                                              reconversion_domain.focus_revision,
                                              output_application_generation);
}

bool UndoCommint(TipTextService* text_service, ITfContext* context,
                 TsfFocusSnapshot undo_domain,
                 uint64_t output_application_generation) {
  if (context == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    // This is an unmanaged context. It's OK. Nothing to do.
    return true;
  }

  const auto is_output_application_current = [&]() {
    return private_context->IsOutputApplicationForFocusDomain(
        undo_domain.focus_epoch, undo_domain.focus_revision,
        output_application_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context, undo_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }

  Output output;
  {
    SessionCommand command;
    command.set_type(SessionCommand::UNDO);
    if (!SendTsfCommand(text_service, context, private_context->GetClient(),
                        command, &output, undo_domain,
                        /*require_nonsecure=*/true,
                        &output_application_generation) ||
        !IsContextFocusedForSnapshot(text_service, context, undo_domain,
                                     /*require_nonsecure=*/true)) {
      return false;
    }
  }

  if (!output.has_deletion_range()) {
    return false;
  }

  const DeletionRange& deletion_range = output.deletion_range();
  const std::optional<std::size_t> deletion_length =
      GetValidPrecedingDeletionLength(deletion_range);
  if (!deletion_length.has_value()) {
    return false;
  }
  if (!TipSurroundingText::DeletePrecedingText(
          text_service, context, *deletion_length, undo_domain.focus_epoch,
          undo_domain.focus_revision, output_application_generation)) {
    if (!IsContextFocusedForSnapshot(text_service, context, undo_domain,
                                     /*require_nonsecure=*/true) ||
        !is_output_application_current()) {
      return false;
    }
    // If TSF-based delete-preceding-text fails, use backspace forwarding as
    // a fall back.

    // Make sure the pending output does not have |deletion_range|.
    // Otherwise, an infinite loop will be created.
    Output pending_output = output;
    pending_output.clear_deletion_range();

    // actually |next_state| will be ignored in TSF Mozc.
    // So it is OK to pass the default value.
    InputState next_state;
    // BeginDeletion can synchronously inject key events. Publish the producer
    // domain before the first synthetic event can observe pending_output().
    private_context->SetPendingOutputFocusDomain(
        undo_domain.focus_epoch, undo_domain.focus_revision,
        output_application_generation);
    private_context->GetDeleter()->BeginDeletion(deletion_range.length(),
                                                 pending_output, next_state);
    return true;
  }

  if (!IsContextFocusedForSnapshot(text_service, context, undo_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }

  if (output.has_callback() && output.callback().has_session_command() &&
      output.callback().session_command().has_type()) {
    // do not allow recursive call.
    return false;
  }

  // Undo commit should be called from and only from the key event handler.
  return TipEditSession::OnOutputReceivedSync(text_service, context,
                                              std::move(output),
                                              undo_domain.focus_epoch,
                                              undo_domain.focus_revision,
                                              output_application_generation);
}

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class SyncEditSessionImpl final : public TipComImplements<ITfEditSession> {
 public:
  SyncEditSessionImpl(wil::com_ptr_nothrow<TipTextService> text_service,
                      wil::com_ptr_nothrow<ITfContext> context, Output output,
                      TsfFocusSnapshot output_domain,
                      uint64_t output_application_generation)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        output_(std::move(output)),
        output_domain_(output_domain),
        output_application_generation_(output_application_generation) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie write_cookie) override {
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     output_domain_,
                                     /*require_nonsecure=*/false)) {
      return S_OK;
    }
    const std::shared_ptr<TipPrivateContext> private_context =
        text_service_->GetPrivateContext(context_.get());
    if (!private_context ||
        !private_context->IsOutputApplicationForFocusDomain(
            output_domain_.focus_epoch, output_domain_.focus_revision,
            output_application_generation_)) {
      return S_OK;
    }
    return TipEditSessionImpl::UpdateContext(
        text_service_.get(), context_.get(), write_cookie, output_,
        output_domain_.focus_epoch, output_domain_.focus_revision,
        output_application_generation_);
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  Output output_;
  TsfFocusSnapshot output_domain_;
  uint64_t output_application_generation_;
};

enum EditSessionMode {
  kDontCare = 0,
  kAsync,
  kSync,
};

bool OnOutputReceivedImpl(TipTextService* text_service,
                          ITfContext* context,
                          Output new_output,
                          TsfFocusSnapshot output_domain,
                          EditSessionMode mode,
                          uint64_t output_application_generation) {
  if (text_service == nullptr || context == nullptr) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    return false;
  }
  if (output_application_generation == 0) {
    if (!IsContextFocusedForSnapshot(text_service, context, output_domain,
                                     /*require_nonsecure=*/false)) {
      return false;
    }
    output_application_generation =
        private_context->ReserveOutputApplicationForFocusDomain(
            output_domain.focus_epoch, output_domain.focus_revision);
  } else if (!private_context->IsOutputApplicationForFocusDomain(
                 output_domain.focus_epoch, output_domain.focus_revision,
                 output_application_generation)) {
    return false;
  }
  const auto is_output_application_current = [&]() {
    return private_context->IsOutputApplicationForFocusDomain(
        output_domain.focus_epoch, output_domain.focus_revision,
        output_application_generation);
  };
  // The caller captures this token before producing the output. Never rebind
  // an old server/IMM32 result to whichever field happens to be focused when
  // the edit session is requested.
  if (!IsContextFocusedForSnapshot(text_service, context, output_domain,
                                   /*require_nonsecure=*/false) ||
      !is_output_application_current()) {
    return false;
  }
  if (new_output.has_callback() &&
      new_output.callback().has_session_command() &&
      new_output.callback().session_command().has_type()) {
    const Output::Callback& callback = new_output.callback();
    const SessionCommand& session_command = callback.session_command();
    const SessionCommand::CommandType type = session_command.type();

    if (callback.has_delay_millisec() && callback.delay_millisec() > 0) {
      text_service->PostDelayedSessionCommand(
          context, session_command, callback.delay_millisec(),
          output_application_generation);
      if (!is_output_application_current()) {
        return false;
      }

      // The current output itself must still be applied immediately.
      // Only the callback is delayed.
      new_output.clear_callback();
    } else {
      switch (type) {
        case SessionCommand::CONVERT_REVERSE:
          return TurnOnImeAndTryToReconvertFromIme(
              text_service, context, output_domain,
              output_application_generation);
        case SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE:
          if (ReconvertSelectionOrKeepFallbackOutput(text_service, context,
                                                     &new_output, output_domain,
                                                     output_application_generation)) {
            return true;
          }
          if (!is_output_application_current()) {
            return false;
          }
          break;
        case SessionCommand::UNDO:
          return UndoCommint(text_service, context, output_domain,
                             output_application_generation);
        default:
          break;
      }
    }
  }

  auto edit_session = MakeComPtr<SyncEditSessionImpl>(
      text_service, context, std::move(new_output), output_domain,
      output_application_generation);

  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context || !is_output_application_current()) {
    return false;
  }

  if (mode == kSync && thread_context->use_async_lock_in_key_handler()) {
    // A workaround for MS Word's failure mode.
    // See https://github.com/google/mozc/issues/819 for details.
    // TODO(https://github.com/google/mozc/issues/821): Remove this workaround.
    mode = kAsync;
  }

  DWORD edit_session_flag = TF_ES_READWRITE;
  switch (mode) {
    case kAsync:
      edit_session_flag |= TF_ES_ASYNC;
      break;
    case kSync:
      edit_session_flag |= TF_ES_SYNC;
      break;
    case kDontCare:
      edit_session_flag |= TF_ES_ASYNCDONTCARE;
      break;
    default:
      DCHECK(false) << "unknown mode: " << mode;
      break;
  }

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = context->RequestEditSession(
      text_service->GetClientID(), edit_session.get(), edit_session_flag,
      &edit_session_result);
  if (!is_output_application_current()) {
    return false;
  }

  if (mode == kSync && edit_session_result == TF_E_SYNCHRONOUS) {
    // A workaround for MS Word's failure mode.
    // See https://github.com/google/mozc/issues/819 for details.
    // TODO(https://github.com/google/mozc/issues/821): Remove this workaround.
    thread_context->set_use_async_lock_in_key_handler(true);
  }

  return SUCCEEDED(hr) && SUCCEEDED(edit_session_result);
}

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class SyncGetTextEditSessionImpl final
    : public TipComImplements<ITfEditSession> {
 public:
  SyncGetTextEditSessionImpl(wil::com_ptr_nothrow<TipTextService> text_service,
                             wil::com_ptr_nothrow<ITfRange> range,
                             wil::com_ptr_nothrow<ITfContext> context,
                             bool check_composition,
                             TsfFocusSnapshot text_domain)
      : text_service_(std::move(text_service)),
        range_(std::move(range)),
        context_(std::move(context)),
        check_composition_(check_composition),
        text_domain_(text_domain) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie read_cookie) override {
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     text_domain_,
                                     /*require_nonsecure=*/true)) {
      return S_OK;
    }
    HRESULT result = TipRangeUtil::GetText(range_.get(), read_cookie, &text_);
    if (FAILED(result)) {
      return result;
    }
    if (!IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                     text_domain_,
                                     /*require_nonsecure=*/true)) {
      text_.clear();
      return S_OK;
    }
    if (check_composition_) {
      wil::com_ptr_nothrow<ITfCompositionView> composition_view =
          TipCompositionUtil::GetCompositionViewFromRange(range_.get(),
                                                          read_cookie);
      is_composing_ = !!composition_view;
    }
    domain_current_ = IsContextFocusedForSnapshot(
        text_service_.get(), context_.get(), text_domain_,
        /*require_nonsecure=*/true);
    if (!domain_current_) {
      text_.clear();
      is_composing_ = false;
    }
    return S_OK;
  }

  const std::wstring& text() const { return text_; }
  bool is_composing() const { return is_composing_; }
  bool domain_current() const { return domain_current_; }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfRange> range_;
  wil::com_ptr_nothrow<ITfContext> context_;
  bool check_composition_ = false;
  TsfFocusSnapshot text_domain_;
  std::wstring text_;
  bool is_composing_ = false;
  bool domain_current_ = false;
};

// This class is an implementation class for the ITfEditSession classes, which
// is an observer for exclusively updating the text store of a TSF thread
// manager.
class AsyncSetTextEditSessionImpl final
    : public TipComImplements<ITfEditSession> {
 public:
  AsyncSetTextEditSessionImpl(wil::com_ptr_nothrow<TipTextService> text_service,
                              const std::wstring_view text,
                              wil::com_ptr_nothrow<ITfRange> range,
                              wil::com_ptr_nothrow<ITfContext> context,
                              TsfFocusSnapshot text_domain,
                              std::shared_ptr<TipPrivateContext> private_context,
                              uint64_t output_application_generation)
      : text_service_(std::move(text_service)),
        text_(text),
        range_(std::move(range)),
        context_(std::move(context)),
        text_domain_(text_domain),
        private_context_(std::move(private_context)),
        output_application_generation_(output_application_generation) {}

  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie write_cookie) override {
    const auto is_application_current = [&]() {
      return IsContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                         text_domain_,
                                         /*require_nonsecure=*/true) &&
             private_context_->IsOutputApplicationForFocusDomain(
                 text_domain_.focus_epoch, text_domain_.focus_revision,
                 output_application_generation_);
    };
    if (!is_application_current()) {
      return S_OK;
    }
    const HRESULT result =
        range_->SetText(write_cookie, 0, text_.data(), text_.size());
    if (!is_application_current()) {
      return S_OK;
    }
    return result;
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  const std::wstring text_;
  wil::com_ptr_nothrow<ITfRange> range_;
  wil::com_ptr_nothrow<ITfContext> context_;
  TsfFocusSnapshot text_domain_;
  std::shared_ptr<TipPrivateContext> private_context_;
  uint64_t output_application_generation_ = 0;
};

}  // namespace

bool TipEditSession::OnOutputReceivedSync(TipTextService* text_service,
                                          ITfContext* context,
                                          Output new_output,
                                          uint64_t output_focus_epoch,
                                          int32_t output_focus_revision,
                                          uint64_t output_application_generation) {
  return OnOutputReceivedImpl(text_service, context, std::move(new_output),
                              {.focus_epoch = output_focus_epoch,
                               .focus_revision = output_focus_revision},
                              kSync, output_application_generation);
}

bool TipEditSession::OnOutputReceivedAsync(TipTextService* text_service,
                                           ITfContext* context,
                                           Output new_output,
                                           uint64_t output_focus_epoch,
                                           int32_t output_focus_revision,
                                           uint64_t output_application_generation) {
  return OnOutputReceivedImpl(text_service, context, std::move(new_output),
                              {.focus_epoch = output_focus_epoch,
                               .focus_revision = output_focus_revision},
                              kAsync, output_application_generation);
}

bool TipEditSession::OnLayoutChangedAsync(TipTextService* text_service,
                                          ITfContext* context) {
  return OnLayoutChangedAsyncImpl(text_service, context);
}

bool TipEditSession::OnSetFocusAsync(TipTextService* text_service,
                                     ITfDocumentMgr* document_manager,
                                     uint64_t focus_epoch,
                                     int32_t focus_revision) {
  const TsfFocusSnapshot scheduled_domain = {
      .focus_epoch = focus_epoch,
      .focus_revision = focus_revision,
  };
  if (document_manager == nullptr) {
    TipUiHandler::OnFocusChange(text_service, nullptr,
                                scheduled_domain.focus_epoch,
                                scheduled_domain.focus_revision);
    return true;
  }
  wil::com_ptr_nothrow<ITfContext> context;
  if (FAILED(document_manager->GetTop(&context)) ||
      !IsContextFocusedForSnapshot(text_service, context.get(),
                                   scheduled_domain,
                                   /*require_nonsecure=*/false)) {
    return false;
  }

  // When RequestEditSession fails, it does not maintain the reference count.
  // So we need to ensure that AddRef/Release should be called at least once
  // per object.
  auto edit_session = MakeComPtr<AsyncSetFocusEditSessionImpl>(
      text_service, context, scheduled_domain.focus_epoch,
      scheduled_domain.focus_revision);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = context->RequestEditSession(
      text_service->GetClientID(), edit_session.get(),
      TF_ES_ASYNCDONTCARE | TF_ES_READ, &edit_session_result);
  return SUCCEEDED(hr) && SUCCEEDED(edit_session_result);
}

bool TipEditSession::OnModeChangedAsync(
    TipTextService* text_service, uint64_t mode_callback_generation) {
  if (text_service == nullptr ||
      !text_service->IsModeCallbackCurrent(
          TipModeCallbackKind::kConversion,
          mode_callback_generation)) {
    return false;
  }
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  wil::com_ptr_nothrow<ITfThreadMgr> thread_mgr =
      text_service->GetThreadManager();
  if (!thread_context || !thread_mgr) {
    return false;
  }

  const TsfFocusSnapshot mode_domain =
      CaptureTsfFocusSnapshot(thread_context.get());
  const auto is_mode_callback_current = [&]() {
    return text_service->IsModeCallbackCurrent(
               TipModeCallbackKind::kConversion,
               mode_callback_generation) &&
           text_service->GetThreadContextLease().get() == thread_context.get();
  };
  wil::com_ptr_nothrow<ITfDocumentMgr> document_manager;
  if (FAILED(thread_mgr->GetFocus(&document_manager)) ||
      !is_mode_callback_current()) {
    return false;
  }
  if (document_manager == nullptr) {
    // This is an unmanaged context. It's OK. Nothing to do.
    return is_mode_callback_current();
  }

  wil::com_ptr_nothrow<ITfContext> context;
  const auto is_mode_transaction_current = [&]() {
    return IsContextFocusedForSnapshot(text_service, context.get(), mode_domain,
                                       /*require_nonsecure=*/false) &&
           is_mode_callback_current();
  };
  if (FAILED(document_manager->GetTop(&context)) || context == nullptr ||
      !is_mode_transaction_current()) {
    return false;
  }
  DWORD native_mode = false;
  if (!TipStatus::GetInputModeConversion(thread_mgr.get(),
                                         text_service->GetClientID(),
                                         &native_mode) ||
      !is_mode_transaction_current()) {
    return false;
  }
  if (!is_mode_transaction_current()) {
    return false;
  }
  const auto action = thread_context->GetInputModeManager()
                          ->OnChangeConversionMode(native_mode);
  if (!is_mode_transaction_current()) {
    return false;
  }
  if (action == TipInputModeManager::kUpdateUI) {
    return OnLayoutChangedAsyncImpl(text_service, context.get(), mode_domain,
                                    mode_callback_generation,
                                    TipModeCallbackKind::kConversion);
  }
  return true;
}

bool TipEditSession::OnOpenCloseChangedAsync(
    TipTextService* text_service, uint64_t mode_callback_generation) {
  if (text_service == nullptr ||
      !text_service->IsModeCallbackCurrent(
          TipModeCallbackKind::kOpenClose,
          mode_callback_generation)) {
    return false;
  }
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  wil::com_ptr_nothrow<ITfThreadMgr> thread_manager =
      text_service->GetThreadManager();
  if (!thread_context || !thread_manager) {
    return false;
  }
  const TsfFocusSnapshot mode_domain =
      CaptureTsfFocusSnapshot(thread_context.get());
  const auto is_mode_callback_current = [&]() {
    return text_service->IsModeCallbackCurrent(
               TipModeCallbackKind::kOpenClose,
               mode_callback_generation) &&
           text_service->GetThreadContextLease().get() == thread_context.get();
  };
  wil::com_ptr_nothrow<ITfDocumentMgr> document_manager;
  if (FAILED(thread_manager->GetFocus(&document_manager)) ||
      !is_mode_callback_current()) {
    return false;
  }
  if (document_manager == nullptr) {
    // This is an unmanaged context. It's OK. Nothing to do.
    return is_mode_callback_current();
  }

  wil::com_ptr_nothrow<ITfContext> context;
  const auto is_mode_transaction_current = [&]() {
    return IsContextFocusedForSnapshot(text_service, context.get(), mode_domain,
                                       /*require_nonsecure=*/false) &&
           is_mode_callback_current();
  };
  if (FAILED(document_manager->GetTop(&context)) || context == nullptr ||
      !is_mode_transaction_current()) {
    return false;
  }
  const bool open = TipStatus::IsOpen(thread_manager.get());
  if (!is_mode_transaction_current()) {
    return false;
  }
  return OnUpdateOnOffModeAsync(text_service, context.get(), open,
                                mode_domain, mode_callback_generation);
}

bool TipEditSession::OnRendererCallbackAsync(TipTextService* text_service,
                                             ITfContext* context,
                                             CommandType type,
                                             int32_t candidate_id,
                                             uint64_t renderer_focus_epoch,
                                             int32_t renderer_focus_revision,
                                             uint64_t output_generation) {
  const TsfFocusSnapshot renderer_domain = {
      .focus_epoch = renderer_focus_epoch,
      .focus_revision = renderer_focus_revision,
  };
  if (!IsContextFocusedForSnapshot(text_service, context, renderer_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (private_context == nullptr ||
      !private_context->IsLastOutputForFocusDomainAndGeneration(
          renderer_domain.focus_epoch, renderer_domain.focus_revision,
          output_generation)) {
    return false;
  }
  switch (commands::GetRendererCallbackCandidateDisposition(
      type, private_context->last_output(), candidate_id)) {
    case commands::RendererCallbackCandidateDisposition::kReject:
      return false;
    case commands::RendererCallbackCandidateDisposition::kAlreadyFocused:
      // Already focused. Nothing to do.
      return true;
    case commands::RendererCallbackCandidateDisposition::kDispatch:
      break;
  }

  SessionCommand command;
  command.set_type(type);
  command.set_id(candidate_id);
  return OnSessionCommandAsync(text_service, context, command,
                               renderer_domain,
                               output_generation,
                               /*expected_output_application_generation=*/0,
                               /*expected_composition_generation=*/0);
}

bool TipEditSession::OnUiElementCallbackAsync(
    TipTextService* text_service, ITfContext* context, CommandType type,
    int32_t candidate_id, uint64_t element_focus_epoch,
    int32_t element_focus_revision, uint64_t output_generation) {
  const TsfFocusSnapshot element_domain = {
      .focus_epoch = element_focus_epoch,
      .focus_revision = element_focus_revision,
  };
  if (!IsContextFocusedForSnapshot(text_service, context, element_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context ||
      !private_context->IsLastOutputForFocusDomainAndGeneration(
          element_focus_epoch, element_focus_revision, output_generation)) {
    return false;
  }
  switch (commands::GetRendererCallbackCandidateDisposition(
      type, private_context->last_output(), candidate_id)) {
    case commands::RendererCallbackCandidateDisposition::kReject:
      return false;
    case commands::RendererCallbackCandidateDisposition::kAlreadyFocused:
      return true;
    case commands::RendererCallbackCandidateDisposition::kDispatch:
      break;
  }
  SessionCommand command;
  command.set_type(type);
  command.set_id(candidate_id);
  return OnSessionCommandAsync(text_service, context, command, element_domain,
                               output_generation,
                               /*expected_output_application_generation=*/0,
                               /*expected_composition_generation=*/0);
}

bool TipEditSession::SendSessionCommandAsync(
    TipTextService* text_service,
    ITfContext* context,
    const commands::SessionCommand& session_command) {
  return OnSessionCommandAsync(text_service, context, session_command);
}

bool TipEditSession::SendSessionCommandAsync(
    TipTextService* text_service, ITfContext* context,
    const commands::SessionCommand& session_command,
    uint64_t command_focus_epoch, int32_t command_focus_revision) {
  return OnSessionCommandAsync(
      text_service, context, session_command,
      {.focus_epoch = command_focus_epoch,
       .focus_revision = command_focus_revision},
      /*expected_output_generation=*/0,
      /*expected_output_application_generation=*/0,
      /*expected_composition_generation=*/0);
}

bool TipEditSession::SendDelayedSessionCommandAsync(
    TipTextService* text_service, ITfContext* context,
    const commands::SessionCommand& session_command,
    uint64_t command_focus_epoch, int32_t command_focus_revision,
    uint64_t composition_generation) {
  return OnSessionCommandAsync(
      text_service, context, session_command,
      {.focus_epoch = command_focus_epoch,
       .focus_revision = command_focus_revision},
      /*expected_output_generation=*/0,
      /*expected_output_application_generation=*/0, composition_generation);
}

bool TipEditSession::SendCompositionSessionCommandAsync(
    TipTextService* text_service, ITfContext* context,
    const commands::SessionCommand& session_command,
    uint64_t command_focus_epoch, int32_t command_focus_revision,
    uint64_t composition_generation) {
  return OnSessionCommandAsync(
      text_service, context, session_command,
      {.focus_epoch = command_focus_epoch,
       .focus_revision = command_focus_revision},
      /*expected_output_generation=*/0,
      /*expected_output_application_generation=*/0, composition_generation);
}

bool TipEditSession::SendUiElementSessionCommandAsync(
    TipTextService* text_service, ITfContext* context,
    const commands::SessionCommand& session_command,
    uint64_t command_focus_epoch, int32_t command_focus_revision,
    uint64_t output_generation) {
  return OnSessionCommandAsync(
      text_service, context, session_command,
      {.focus_epoch = command_focus_epoch,
       .focus_revision = command_focus_revision},
      output_generation,
      /*expected_output_application_generation=*/0,
      /*expected_composition_generation=*/0);
}

bool TipEditSession::SubmitAsync(TipTextService* text_service,
                                 ITfContext* context) {
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    // This is an unmanaged context.
    return false;
  }

  SessionCommand session_command;
  session_command.set_type(SessionCommand::SUBMIT);
  return OnSessionCommandAsync(text_service, context, session_command);
}

bool TipEditSession::CancelCompositionAsync(TipTextService* text_service,
                                            ITfContext* context) {
  SessionCommand command;
  command.set_type(SessionCommand::REVERT);
  return OnSessionCommandAsync(text_service, context, command);
}

bool TipEditSession::HilightCandidateAsync(TipTextService* text_service,
                                           ITfContext* context,
                                           int candidate_id) {
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    // This is an unmanaged context.
    return false;
  }

  SessionCommand session_command;
  session_command.set_type(SessionCommand::HIGHLIGHT_CANDIDATE);
  session_command.set_id(candidate_id);
  return OnSessionCommandAsync(text_service, context, session_command);
}

bool TipEditSession::SelectCandidateAsync(TipTextService* text_service,
                                          ITfContext* context,
                                          int candidate_id) {
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (!private_context) {
    // This is an unmanaged context.
    return false;
  }

  SessionCommand session_command;
  session_command.set_type(SessionCommand::SELECT_CANDIDATE);
  session_command.set_id(candidate_id);
  return OnSessionCommandAsync(text_service, context, session_command);
}

bool TipEditSession::ReconvertFromApplicationSync(TipTextService* text_service,
                                                  ITfRange* range) {
  if (text_service == nullptr || range == nullptr) {
    return false;
  }
  const TsfFocusSnapshot reconversion_domain =
      CaptureTsfFocusSnapshot(text_service->GetThreadContext());
  wil::com_ptr_nothrow<ITfContext> context;
  if (FAILED(range->GetContext(&context))) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context.get());
  if (!private_context) {
    // This is an unmanaged context.
    return false;
  }
  if (!IsContextFocusedForSnapshot(text_service, context.get(),
                                   reconversion_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  uint64_t output_application_generation =
      private_context->ReserveOutputApplicationForFocusDomain(
          reconversion_domain.focus_epoch, reconversion_domain.focus_revision);
  const auto is_output_application_current = [&]() {
    return private_context->IsOutputApplicationForFocusDomain(
        reconversion_domain.focus_epoch, reconversion_domain.focus_revision,
        output_application_generation);
  };
  if (!IsContextFocusedForSnapshot(text_service, context.get(),
                                   reconversion_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }

  std::wstring selected_text;
  bool is_composing = false;
  if (!GetTextSync(text_service, range, &selected_text, &is_composing,
                   reconversion_domain.focus_epoch,
                   reconversion_domain.focus_revision) ||
      !IsContextFocusedForSnapshot(text_service, context.get(),
                                   reconversion_domain,
                                   /*require_nonsecure=*/true) ||
      !is_output_application_current()) {
    return false;
  }

  if (selected_text.empty()) {
    // Selected text is empty. Nothing to do.
    return false;
  }

  if (is_composing) {
    // on-going composition is found.
    return false;
  }

  // Stop reconversion when any embedded object is found because we cannot
  // easily restore it. See b/3406434
  if (selected_text.find(static_cast<wchar_t>(TS_CHAR_EMBEDDED)) !=
      std::wstring::npos) {
    // embedded object is found.
    return false;
  }

  const std::string selected_text_utf8 = WideToUtf8(selected_text);
  Output output;
  if (!SendConvertReverseForFocus(
          text_service, context.get(), private_context->GetClient(),
          selected_text_utf8, reconversion_domain, &output,
          &output_application_generation)) {
    return false;
  }
  return OnOutputReceivedSync(text_service, context.get(), std::move(output),
                              reconversion_domain.focus_epoch,
                              reconversion_domain.focus_revision,
                              output_application_generation);
}

bool TipEditSession::SwitchInputModeAsync(TipTextService* text_service,
                                          uint32_t mozc_mode) {
  commands::CompositionMode mode =
      static_cast<commands::CompositionMode>(mozc_mode);

  if (text_service == nullptr) {
    return false;
  }
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  wil::com_ptr_nothrow<ITfThreadMgr> thread_mgr =
      text_service->GetThreadManager();
  if (!thread_context || !thread_mgr) {
    return false;
  }

  const TsfFocusSnapshot mode_domain =
      CaptureTsfFocusSnapshot(thread_context.get());
  const uint64_t output_application_generation =
      text_service->ReserveActiveOutputApplication(
          mode_domain.focus_epoch, mode_domain.focus_revision);
  if (output_application_generation == 0) {
    return false;
  }
  wil::com_ptr_nothrow<ITfDocumentMgr> document;
  if (FAILED(thread_mgr->GetFocus(&document))) {
    return false;
  }
  if (text_service->GetThreadContextLease().get() != thread_context.get()) {
    return false;
  }
  if (document == nullptr) {
    // This is an unmanaged context. It's OK. Nothing to do.
    return true;
  }

  wil::com_ptr_nothrow<ITfContext> context;
  if (FAILED(document->GetTop(&context)) ||
      !IsContextFocusedForSnapshot(text_service, context.get(), mode_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }

  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context.get());
  const auto is_output_application_current = [&]() {
    return private_context != nullptr &&
           IsContextFocusedForSnapshot(text_service, context.get(), mode_domain,
                                       /*require_nonsecure=*/true) &&
           private_context->IsOutputApplicationForFocusDomain(
               mode_domain.focus_epoch, mode_domain.focus_revision,
               output_application_generation);
  };
  if (!is_output_application_current()) {
    return false;
  }

  if (mode == commands::DIRECT) {
    DWORD native_mode = 0;
    if (!TipStatus::GetInputModeConversion(thread_mgr.get(),
                                           text_service->GetClientID(),
                                           &native_mode) ||
        !is_output_application_current()) {
      return false;
    }
    return OnSwitchInputModeAsync(text_service, context.get(), false,
                                  native_mode, mode_domain,
                                  output_application_generation);
  }
  if (!private_context) {
    // This is an unmanaged context.
    return false;
  }

  uint32_t native_mode = 0;
  if (!ConversionModeUtil::ToNativeMode(
          mode, private_context->input_behavior().prefer_kana_input,
          &native_mode)) {
    return false;
  }

  if (!is_output_application_current()) {
    return false;
  }
  return OnSwitchInputModeAsync(text_service, context.get(), true, native_mode,
                                mode_domain, output_application_generation);
}

bool TipEditSession::GetTextSync(TipTextService* text_service, ITfRange* range,
                                 std::wstring* text, bool* is_composing,
                                 uint64_t text_focus_epoch,
                                 int32_t text_focus_revision) {
  if (text_service == nullptr || range == nullptr || text == nullptr) {
    return false;
  }
  const TsfFocusSnapshot text_domain = {
      .focus_epoch = text_focus_epoch,
      .focus_revision = text_focus_revision,
  };
  wil::com_ptr_nothrow<ITfContext> context;
  if (FAILED(range->GetContext(&context)) ||
      !IsContextFocusedForSnapshot(text_service, context.get(), text_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  const bool check_composition = (is_composing != nullptr);
  auto get_text = MakeComPtr<SyncGetTextEditSessionImpl>(
      text_service, range, context, check_composition, text_domain);

  HRESULT hr = S_OK;
  HRESULT hr_session = S_OK;
  hr = context->RequestEditSession(text_service->GetClientID(), get_text.get(),
                                   TF_ES_SYNC | TF_ES_READ, &hr_session);
  if (FAILED(hr) || FAILED(hr_session) || !get_text->domain_current() ||
      !IsContextFocusedForSnapshot(text_service, context.get(), text_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  *text = get_text->text();
  if (check_composition) {
    *is_composing = get_text->is_composing();
  }
  return true;
}

// static
bool TipEditSession::SetTextAsync(TipTextService* text_service,
                                  const std::wstring_view text,
                                  ITfRange* range,
                                  uint64_t text_focus_epoch,
                                  int32_t text_focus_revision,
                                  uint64_t output_application_generation) {
  if (text_service == nullptr || range == nullptr) {
    return false;
  }
  const TsfFocusSnapshot text_domain = {
      .focus_epoch = text_focus_epoch,
      .focus_revision = text_focus_revision,
  };
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context || !IsNonSecureTsfFocusSnapshotCurrent(
                             thread_context.get(), text_domain)) {
    return false;
  }
  wil::com_ptr_nothrow<ITfContext> context;
  if (FAILED(range->GetContext(&context)) ||
      !IsContextFocusedForSnapshot(text_service, context.get(), text_domain,
                                   /*require_nonsecure=*/true)) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context.get());
  const auto is_application_current = [&]() {
    return private_context != nullptr &&
           IsContextFocusedForSnapshot(text_service, context.get(), text_domain,
                                       /*require_nonsecure=*/true) &&
           private_context->IsOutputApplicationForFocusDomain(
               text_domain.focus_epoch, text_domain.focus_revision,
               output_application_generation);
  };
  if (!is_application_current()) {
    return false;
  }
  auto set_text = MakeComPtr<AsyncSetTextEditSessionImpl>(
      text_service, text, range, context, text_domain, private_context,
      output_application_generation);

  HRESULT hr = S_OK;
  HRESULT hr_session = S_OK;
  hr = context->RequestEditSession(text_service->GetClientID(), set_text.get(),
                                   TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                   &hr_session);
  return SUCCEEDED(hr) && SUCCEEDED(hr_session) && is_application_current();
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
