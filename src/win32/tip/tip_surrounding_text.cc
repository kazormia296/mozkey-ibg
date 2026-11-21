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

#include "win32/tip/tip_surrounding_text.h"

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

#include "base/win32/com.h"
#include "base/win32/wide_char.h"
#include "win32/base/imm_reconvert_string.h"
#include "win32/tip/tip_composition_util.h"
#include "win32/tip/tip_dll_module.h"
#include "win32/tip/tip_input_mode_manager.h"
#include "win32/tip/tip_private_context.h"
#include "win32/tip/tip_range_util.h"
#include "win32/tip/tip_text_service.h"
#include "win32/tip/tip_thread_context.h"
#include "win32/tip/tip_transitory_extension.h"

namespace mozc {
namespace win32 {
namespace tsf {
namespace {

constexpr int kDefaultMaxSurroundingLength = 20;
constexpr int kMaxCharacterLength = 1024 * 1024;

struct SurroundingTextFocusSnapshot final {
  uint64_t focus_epoch = 0;
  int32_t focus_revision = 0;
};

bool IsSameComObject(IUnknown* lhs, IUnknown* rhs) {
  const auto lhs_identity = ComQuery<IUnknown>(lhs);
  const auto rhs_identity = ComQuery<IUnknown>(rhs);
  return lhs_identity && rhs_identity &&
         lhs_identity.get() == rhs_identity.get();
}

SurroundingTextFocusSnapshot CaptureSurroundingTextFocusSnapshot(
    TipTextService* text_service) {
  if (text_service == nullptr) {
    return {};
  }
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context) {
    return {};
  }
  return {
      .focus_epoch = thread_context->GetGrimodexFocusEpoch(),
      .focus_revision = thread_context->GetFocusRevision(),
  };
}

bool IsSurroundingTextContextCurrent(
    TipTextService* text_service, ITfContext* expected_context,
    SurroundingTextFocusSnapshot snapshot) {
  if (text_service == nullptr || expected_context == nullptr ||
      snapshot.focus_epoch == 0 || !text_service->HasThreadFocus()) {
    return false;
  }
  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context) {
    return false;
  }
  if (thread_context->GetGrimodexFocusEpoch() != snapshot.focus_epoch ||
      thread_context->GetFocusRevision() != snapshot.focus_revision ||
      thread_context->GetInputModeManager()->IsPasswordInputScope()) {
    return false;
  }
  wil::com_ptr_nothrow<ITfThreadMgr> thread_manager =
      text_service->GetThreadManager();
  if (!thread_manager ||
      text_service->GetThreadContextLease().get() != thread_context.get()) {
    return false;
  }
  wil::com_ptr_nothrow<ITfDocumentMgr> document;
  if (FAILED(thread_manager->GetFocus(&document)) ||
      document == nullptr ||
      text_service->GetThreadContextLease().get() != thread_context.get()) {
    return false;
  }
  wil::com_ptr_nothrow<ITfContext> focused_context;
  if (FAILED(document->GetTop(&focused_context)) || focused_context == nullptr ||
      !IsSameComObject(expected_context, focused_context.get()) ||
      text_service->GetThreadContextLease().get() != thread_context.get()) {
    return false;
  }
  return thread_context->GetGrimodexFocusEpoch() == snapshot.focus_epoch &&
         thread_context->GetFocusRevision() == snapshot.focus_revision &&
         !thread_context->GetInputModeManager()->IsPasswordInputScope() &&
         text_service->GetThreadContextLease().get() == thread_context.get() &&
         text_service->HasThreadFocus();
}

bool IsFullContextMappingCurrent(TipTextService* text_service,
                                 ITfContext* source_context,
                                 ITfContext* full_context,
                                 SurroundingTextFocusSnapshot domain) {
  if (!IsSurroundingTextContextCurrent(text_service, source_context, domain)) {
    return false;
  }
  const wil::com_ptr_nothrow<ITfContext> current_full_context =
      TipTransitoryExtension::AsFullContext(source_context);
  return current_full_context != nullptr &&
         IsSameComObject(current_full_context.get(), full_context) &&
         IsSurroundingTextContextCurrent(text_service, source_context, domain);
}

class SurroudingTextUpdater final : public TipComImplements<ITfEditSession> {
 public:
  SurroudingTextUpdater(
      wil::com_ptr_nothrow<TipTextService> text_service,
      wil::com_ptr_nothrow<ITfContext> source_context,
      wil::com_ptr_nothrow<ITfContext> full_context, bool move_anchor,
      bool retrieve_selected_text, int max_preceding_length,
      int max_following_length, SurroundingTextFocusSnapshot domain)
      : text_service_(std::move(text_service)),
        source_context_(std::move(source_context)),
        context_(std::move(full_context)),
        move_anchor_(move_anchor),
        retrieve_selected_text_(retrieve_selected_text),
        max_preceding_length_(max_preceding_length),
        max_following_length_(max_following_length),
        domain_(domain) {}

  const TipSurroundingTextInfo& result() const { return result_; }
  bool domain_current() const { return domain_current_; }

 private:
  STDMETHODIMP DoEditSession(TfEditCookie edit_cookie) override {
    if (!IsFullContextMappingCurrent(text_service_.get(), source_context_.get(),
                                     context_.get(), domain_)) {
      return S_OK;
    }
    HRESULT result = S_OK;
    {
      wil::com_ptr_nothrow<ITfCompositionView> composition_view =
          TipCompositionUtil::GetCompositionView(context_.get(), edit_cookie);
      result_.in_composition = !!composition_view;
    }

    wil::com_ptr_nothrow<ITfRange> selected_range;
    {
      result = TipRangeUtil::GetDefaultSelection(context_.get(), edit_cookie,
                                                 &selected_range, nullptr);
      if (FAILED(result)) {
        return result;
      }

      // Inspect the current selection's InputScope in this same synchronous
      // read edit session, before retrieving any selected/preceding/following
      // text. This avoids relying on the asynchronous focus-scope cache.
      std::vector<InputScope> input_scopes;
      if (SUCCEEDED(TipRangeUtil::GetInputScopes(
              selected_range.get(), edit_cookie, &input_scopes))) {
        result_.is_private_input_scope =
            TipSurroundingTextUtil::ContainsPrivateInputScope(input_scopes);

        if (TipSurroundingTextUtil::ContainsPasswordInputScope(input_scopes)) {
          // Report the password scope only while the focus domain captured by
          // the caller is still current; otherwise the caller sees a failure.
          domain_current_ = IsFullContextMappingCurrent(
              text_service_.get(), source_context_.get(), context_.get(),
              domain_);
          if (domain_current_) {
            result_.is_password_input_scope = true;
          } else {
            result_ = TipSurroundingTextInfo();
          }
          return S_OK;
        }
      }

      if (retrieve_selected_text_) {
        result = TipRangeUtil::GetText(selected_range.get(), edit_cookie,
                                       &result_.selected_text);
        result_.has_selected_text = SUCCEEDED(result);
      }

      // For reconversion, the active selection end should be moved to the
      // front character.  Do not call SetSelection for an empty selection.
      // Some TSF hosts (notably Gecko) treat even a no-op SetSelection during
      // key handling as evidence that the IME processed the physical key.
      // Avoid a no-op selection update: some hosts treat it as handling the
      // physical key even when there is no selection to reconvert.
      const bool should_move_anchor =
          TipSurroundingTextUtil::ShouldMoveAnchorForReconversion(
              move_anchor_, retrieve_selected_text_, result_);
      if (should_move_anchor) {
        if (!IsFullContextMappingCurrent(
                text_service_.get(), source_context_.get(), context_.get(),
                domain_)) {
          return S_OK;
        }
        result = TipRangeUtil::SetSelection(context_.get(), edit_cookie,
                                            selected_range.get(), TF_AE_START);
        if (FAILED(result)) {
          return result;
        }
        if (!IsFullContextMappingCurrent(
                text_service_.get(), source_context_.get(), context_.get(),
                domain_)) {
          return S_OK;
        }
      }
    }

    const TF_HALTCOND halt_cond = {nullptr, TF_ANCHOR_START, TF_HF_OBJECT};

    if (max_preceding_length_ > 0) {
      wil::com_ptr_nothrow<ITfRange> preceding_range;
      LONG preceding_range_shifted = 0;
      if (SUCCEEDED(selected_range->Clone(&preceding_range)) &&
          SUCCEEDED(preceding_range->Collapse(edit_cookie, TF_ANCHOR_START)) &&
          SUCCEEDED(preceding_range->ShiftStart(
              edit_cookie, -max_preceding_length_, &preceding_range_shifted,
              &halt_cond))) {
        HRESULT result = TipRangeUtil::GetText(
            preceding_range.get(), edit_cookie, &result_.preceding_text);
        result_.has_preceding_text = SUCCEEDED(result);
      }
    }

    if (max_following_length_ > 0) {
      wil::com_ptr_nothrow<ITfRange> following_range;
      LONG following_range_shifted = 0;
      if (SUCCEEDED(selected_range->Clone(&following_range)) &&
          SUCCEEDED(following_range->Collapse(edit_cookie, TF_ANCHOR_END)) &&
          SUCCEEDED(following_range->ShiftEnd(
              edit_cookie, max_following_length_, &following_range_shifted,
              &halt_cond))) {
        HRESULT result = TipRangeUtil::GetText(
            following_range.get(), edit_cookie, &result_.following_text);
        result_.has_following_text = SUCCEEDED(result);
      }
    }

    domain_current_ = IsFullContextMappingCurrent(
        text_service_.get(), source_context_.get(), context_.get(), domain_);
    if (!domain_current_) {
      result_ = TipSurroundingTextInfo();
    }
    return S_OK;
  }

  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> source_context_;
  wil::com_ptr_nothrow<ITfContext> context_;
  TipSurroundingTextInfo result_;
  bool move_anchor_;
  bool retrieve_selected_text_;
  int max_preceding_length_;
  int max_following_length_;
  SurroundingTextFocusSnapshot domain_;
  bool domain_current_ = false;
};

class PrecedingTextDeleter final : public TipComImplements<ITfEditSession> {
 public:
  PrecedingTextDeleter(
      wil::com_ptr_nothrow<TipTextService> text_service,
      wil::com_ptr_nothrow<ITfContext> source_context,
      wil::com_ptr_nothrow<ITfContext> full_context,
      size_t num_characters_in_codepoint,
      SurroundingTextFocusSnapshot domain,
      uint64_t output_application_generation)
      : text_service_(std::move(text_service)),
        source_context_(std::move(source_context)),
        context_(std::move(full_context)),
        num_characters_in_codepoint_(num_characters_in_codepoint),
        domain_(domain),
        output_application_generation_(output_application_generation) {}

  bool domain_current() const { return domain_current_; }

 private:
  STDMETHODIMP DoEditSession(TfEditCookie edit_cookie) override {
    const auto is_current = [&]() {
      if (!IsFullContextMappingCurrent(text_service_.get(),
                                       source_context_.get(), context_.get(),
                                       domain_)) {
        return false;
      }
      const std::shared_ptr<TipPrivateContext> private_context =
          text_service_->GetPrivateContext(source_context_.get());
      return private_context &&
             private_context->IsOutputApplicationForFocusDomain(
                 domain_.focus_epoch, domain_.focus_revision,
                 output_application_generation_);
    };
    if (!is_current()) {
      return S_OK;
    }
    HRESULT result = S_OK;

    wil::com_ptr_nothrow<ITfRange> selected_range;
    result = TipRangeUtil::GetDefaultSelection(context_.get(), edit_cookie,
                                               &selected_range, nullptr);
    if (FAILED(result)) {
      return result;
    }
    if (!is_current()) {
      return S_OK;
    }

    const TF_HALTCOND halt_cond = {nullptr, TF_ANCHOR_START, 0};

    wil::com_ptr_nothrow<ITfRange> preceding_range;
    if (FAILED(selected_range->Clone(&preceding_range))) {
      return E_FAIL;
    }
    if (!is_current()) {
      return S_OK;
    }
    if (FAILED(preceding_range->Collapse(edit_cookie, TF_ANCHOR_START))) {
      return E_FAIL;
    }
    if (!is_current()) {
      return S_OK;
    }

    // If all the characters are surrogate-pair, |num_characters_in_codepoint_|
    // * 2 is required.
    if (num_characters_in_codepoint_ >= kMaxCharacterLength) {
      return E_UNEXPECTED;
    }
    const LONG initial_offset_utf16 =
        -static_cast<LONG>(num_characters_in_codepoint_) * 2;
    LONG preceding_range_shifted = 0;
    if (FAILED(preceding_range->ShiftStart(edit_cookie, initial_offset_utf16,
                                           &preceding_range_shifted,
                                           &halt_cond))) {
      return E_FAIL;
    }
    if (!is_current()) {
      return S_OK;
    }
    std::wstring total_string;
    if (FAILED(TipRangeUtil::GetText(preceding_range.get(), edit_cookie,
                                     &total_string))) {
      return E_FAIL;
    }
    if (!is_current()) {
      return S_OK;
    }
    if (total_string.empty()) {
      return E_FAIL;
    }

    size_t len_in_utf16 = 0;
    if (!TipSurroundingTextUtil::MeasureCharactersBackward(
            total_string, num_characters_in_codepoint_, &len_in_utf16)) {
      return E_FAIL;
    }

    const LONG final_offset = total_string.size() - len_in_utf16;
    if (FAILED(preceding_range->ShiftStart(
            edit_cookie, final_offset, &preceding_range_shifted, &halt_cond))) {
      return E_FAIL;
    }
    if (!is_current()) {
      return S_OK;
    }
    if (final_offset != preceding_range_shifted) {
      return E_FAIL;
    }
    if (!is_current() ||
        FAILED(preceding_range->SetText(edit_cookie, 0, L"", 0))) {
      return E_FAIL;
    }
    domain_current_ = is_current();
    return S_OK;
  }

  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> source_context_;
  wil::com_ptr_nothrow<ITfContext> context_;
  size_t num_characters_in_codepoint_;
  SurroundingTextFocusSnapshot domain_;
  uint64_t output_application_generation_;
  bool domain_current_ = false;
};

bool GetSurroundingTextImm32(ITfContext* context,
                             ReconvertString::RequestType request_type,
                             TipSurroundingTextInfo* info) {
  wil::com_ptr_nothrow<ITfContextView> context_view;
  if (FAILED(context->GetActiveView(&context_view))) {
    return false;
  }
  if (context_view == nullptr) {
    return false;
  }
  HWND attached_window = nullptr;
  if (FAILED(context_view->GetWnd(&attached_window))) {
    return false;
  }

  UniqueReconvertString reconvert_string =
      ReconvertString::Request(attached_window, request_type);
  if (!reconvert_string) {
    return false;
  }

  std::optional<ReconvertString::Strings> ss = reconvert_string->Decompose();
  if (!ss.has_value()) {
    return false;
  }
  info->in_composition = false;
  info->used_legacy_imm32_fallback = true;
  info->has_preceding_text = true;
  info->preceding_text.assign(ss->preceding_text.begin(),
                              ss->preceding_text.end());
  info->has_selected_text = true;
  info->selected_text =
      StrCatW(ss->preceding_composition, ss->target, ss->following_composition);
  info->has_following_text = true;
  info->following_text.assign(ss->following_text.begin(),
                              ss->following_text.end());

  return true;
}

}  // namespace

bool TipSurroundingText::Get(TipTextService* text_service, ITfContext* context,
                             TipSurroundingTextInfo* info) {
  if (text_service == nullptr || context == nullptr || info == nullptr) {
    return false;
  }
  *info = TipSurroundingTextInfo();
  const SurroundingTextFocusSnapshot domain =
      CaptureSurroundingTextFocusSnapshot(text_service);
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }

  // Surrounding text retrieval through TSF APIs should be performed only with
  // the full context.
  wil::com_ptr_nothrow<ITfContext> full_context(
      TipTransitoryExtension::AsFullContext(context));
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }
  if (full_context == nullptr) {
    // Legacy IMM32-based editors fall into this category.
    // Try to retrieve surrounding text through IMR_DOCUMENTFEED as a fallback.
    const bool result = GetSurroundingTextImm32(
        context, ReconvertString::RequestType::kDocumentFeed, info);
    if (!result ||
        !IsSurroundingTextContextCurrent(text_service, context, domain)) {
      *info = TipSurroundingTextInfo();
      return false;
    }
    return true;
  }

  auto updater = MakeComPtr<SurroudingTextUpdater>(
      text_service, context, full_context, /*move_anchor=*/false,
      /*retrieve_selected_text=*/true, kDefaultMaxSurroundingLength,
      kDefaultMaxSurroundingLength, domain);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = full_context->RequestEditSession(
      text_service->GetClientID(), updater.get(), TF_ES_SYNC | TF_ES_READ,
      &edit_session_result);
  if (FAILED(hr) || FAILED(edit_session_result) ||
      !updater->domain_current()) {
    return false;
  }

  *info = updater->result();
  return true;
}

bool TipSurroundingText::GetForZenzContext(
    TipTextService* text_service, ITfContext* context,
    size_t max_preceding_length, size_t max_following_length,
    TipSurroundingTextInfo* info) {
  if (text_service == nullptr || context == nullptr || info == nullptr) {
    return false;
  }
  *info = TipSurroundingTextInfo();
  const SurroundingTextFocusSnapshot domain =
      CaptureSurroundingTextFocusSnapshot(text_service);
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }

  wil::com_ptr_nothrow<ITfContext> full_context(
      TipTransitoryExtension::AsFullContext(context));
  if (full_context == nullptr ||
      !IsSurroundingTextContextCurrent(text_service, context, domain)) {
    // Zenz deliberately does not use the legacy IMM32 document-feed path.
    return false;
  }

  const int preceding_limit =
      max_preceding_length > static_cast<size_t>(kMaxCharacterLength)
          ? kMaxCharacterLength
          : static_cast<int>(max_preceding_length);
  const int following_limit =
      max_following_length > static_cast<size_t>(kMaxCharacterLength)
          ? kMaxCharacterLength
          : static_cast<int>(max_following_length);
  auto updater = MakeComPtr<SurroudingTextUpdater>(
      text_service, context, full_context, /*move_anchor=*/false,
      /*retrieve_selected_text=*/false, preceding_limit, following_limit,
      domain);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = full_context->RequestEditSession(
      text_service->GetClientID(), updater.get(), TF_ES_SYNC | TF_ES_READ,
      &edit_session_result);
  if (FAILED(hr) || FAILED(edit_session_result) ||
      !updater->domain_current()) {
    return false;
  }

  *info = updater->result();
  return true;
}

bool PrepareForReconversionTSF(TipTextService* text_service,
                               ITfContext* context,
                               TipSurroundingTextInfo* info,
                               SurroundingTextFocusSnapshot domain) {
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }
  // Reconversion through TSF APIs should be performed only with the full
  // context.
  wil::com_ptr_nothrow<ITfContext> full_context(
      TipTransitoryExtension::AsFullContext(context));
  if (full_context == nullptr ||
      !IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }

  // When RequestEditSession fails, it does not maintain the reference count.
  // So we need to ensure that AddRef/Release should be called at least once
  // per object.
  auto updater = MakeComPtr<SurroudingTextUpdater>(
      text_service, context, full_context, /*move_anchor=*/true,
      /*retrieve_selected_text=*/true, kDefaultMaxSurroundingLength,
      kDefaultMaxSurroundingLength, domain);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = full_context->RequestEditSession(
      text_service->GetClientID(), updater.get(), TF_ES_SYNC | TF_ES_READWRITE,
      &edit_session_result);
  if (FAILED(hr)) {
    return false;
  }
  if (FAILED(edit_session_result) || !updater->domain_current()) {
    return false;
  }

  *info = updater->result();
  return true;
}

bool TipSurroundingText::PrepareForExplicitSelectionReconversionFromIme(
    TipTextService* text_service, ITfContext* context,
    TipSurroundingTextInfo* info, uint64_t focus_epoch,
    int32_t focus_revision) {
  if (text_service == nullptr || context == nullptr || info == nullptr) {
    return false;
  }
  *info = TipSurroundingTextInfo();
  const SurroundingTextFocusSnapshot domain = {
      .focus_epoch = focus_epoch,
      .focus_revision = focus_revision,
  };
  // IMR_RECONVERTSTRING can return text near the caret without a selection.
  // Only a successful full-TSF range read can authorize Space reconversion.
  if (!PrepareForReconversionTSF(text_service, context, info, domain)) {
    *info = TipSurroundingTextInfo();
    return false;
  }
  return true;
}

bool TipSurroundingText::PrepareForReconversionFromIme(
    TipTextService* text_service, ITfContext* context,
    TipSurroundingTextInfo* info, bool* need_async_reconversion,
    uint64_t focus_epoch, int32_t focus_revision) {
  if (text_service == nullptr || context == nullptr || info == nullptr ||
      need_async_reconversion == nullptr) {
    return false;
  }
  *info = TipSurroundingTextInfo();
  *need_async_reconversion = false;
  const SurroundingTextFocusSnapshot domain = {
      .focus_epoch = focus_epoch,
      .focus_revision = focus_revision,
  };
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }
  if (PrepareForReconversionTSF(text_service, context, info, domain)) {
    return true;
  }
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    *info = TipSurroundingTextInfo();
    return false;
  }
  if (!GetSurroundingTextImm32(
          context, ReconvertString::RequestType::kReconvertString, info)) {
    // Certain apps such as Excel do start reconversions by using
    // ITfFnReconversion protocol upon receiving IMR_RECONVERTSTRING message,
    // even though they return 0 (== failure) to the message.
    // See https://github.com/google/mozc/issues/1384 for details.
    // In this sense, seeing failure here is still a necessary step to support
    // reconversions in such apps. We just need to wait for the app to call into
    // TipReconvertFunction::Reconvert later.
    return false;
  }
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    *info = TipSurroundingTextInfo();
    return false;
  }
  // IMM32-like reconversion requires async edit session.
  *need_async_reconversion = true;
  return true;
}

bool TipSurroundingText::DeletePrecedingText(
    TipTextService* text_service, ITfContext* context,
    size_t num_characters_to_be_deleted_in_codepoint, uint64_t focus_epoch,
    int32_t focus_revision, uint64_t output_application_generation) {
  if (text_service == nullptr || context == nullptr) {
    return false;
  }
  const SurroundingTextFocusSnapshot domain = {
      .focus_epoch = focus_epoch,
      .focus_revision = focus_revision,
  };
  if (!IsSurroundingTextContextCurrent(text_service, context, domain)) {
    return false;
  }
  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  const auto is_output_application_current = [&]() {
    return private_context &&
           private_context->IsOutputApplicationForFocusDomain(
               domain.focus_epoch, domain.focus_revision,
               output_application_generation);
  };
  if (!is_output_application_current()) {
    return false;
  }
  // Surrounding text deletion through TSF APIs should be performed only with
  // the full context.
  wil::com_ptr_nothrow<ITfContext> full_context(
      TipTransitoryExtension::AsFullContext(context));
  if (full_context == nullptr ||
      !IsSurroundingTextContextCurrent(text_service, context, domain) ||
      !is_output_application_current()) {
    return false;
  }

  // When RequestEditSession fails, it does not maintain the reference count.
  // So we need to ensure that AddRef/Release should be called at least once
  // per object.
  auto edit_session = MakeComPtr<PrecedingTextDeleter>(
      text_service, context, full_context,
      num_characters_to_be_deleted_in_codepoint, domain,
      output_application_generation);

  HRESULT edit_session_result = S_OK;
  const HRESULT hr = full_context->RequestEditSession(
      text_service->GetClientID(), edit_session.get(),
      TF_ES_SYNC | TF_ES_READWRITE, &edit_session_result);
  if (FAILED(hr)) {
    return false;
  }
  if (FAILED(edit_session_result) || !edit_session->domain_current() ||
      !is_output_application_current()) {
    return false;
  }
  return true;
}

bool TipSurroundingTextUtil::ContainsEmbeddedObject(
    const std::wstring_view text) {
  return text.find(static_cast<wchar_t>(TS_CHAR_EMBEDDED)) !=
         std::wstring_view::npos;
}

bool TipSurroundingTextUtil::HasExplicitlySelectedTextForReconversion(
    const TipSurroundingTextInfo& info) {
  return info.has_selected_text && !info.selected_text.empty() &&
         !info.used_legacy_imm32_fallback;
}

bool TipSurroundingTextUtil::ShouldMoveAnchorForReconversion(
    const bool move_anchor, const bool retrieve_selected_text,
    const TipSurroundingTextInfo& info) {
  if (!move_anchor) {
    return false;
  }
  if (!retrieve_selected_text) {
    return true;
  }
  return info.has_selected_text && !info.selected_text.empty() &&
         !ContainsEmbeddedObject(info.selected_text);
}

bool TipSurroundingTextUtil::ContainsPasswordInputScope(
    const std::vector<InputScope>& input_scopes) {
  for (const InputScope input_scope : input_scopes) {
    if (input_scope == IS_PASSWORD) {
      return true;
    }
  }
  return false;
}

bool TipSurroundingTextUtil::ContainsPrivateInputScope(
    const std::vector<InputScope>& input_scopes) {
  for (const InputScope input_scope : input_scopes) {
    if (input_scope == IS_PRIVATE) {
      return true;
    }
  }
  return false;
}

bool TipSurroundingTextUtil::MeasureCharactersBackward(
    const std::wstring_view text, const size_t characters_in_codepoint,
    size_t* characters_in_utf16) {
  if (characters_in_utf16 == nullptr) {
    return false;
  }
  *characters_in_utf16 = 0;

  // Count characters from the end of |text| with taking surrogate pair into
  // consideration. Finally, we will find that |num_char_in_codepoint|
  // characters consist of |checked_len_in_utf16| UTF16 elements.
  size_t checked_len_in_utf16 = 0;
  size_t num_char_in_codepoint = 0;
  while (true) {
    if (num_char_in_codepoint >= characters_in_codepoint) {
      break;
    }
    if (checked_len_in_utf16 + 1 > text.size()) {
      break;
    }
    ++checked_len_in_utf16;
    const size_t index_low = text.size() - checked_len_in_utf16;
    if (IS_LOW_SURROGATE(text[index_low])) {
      if (checked_len_in_utf16 + 1 <= text.size()) {
        const size_t index_high = text.size() - checked_len_in_utf16 - 1;
        if (IS_HIGH_SURROGATE(text[index_high])) {
          ++checked_len_in_utf16;
        }
      }
    }
    ++num_char_in_codepoint;
  }

  if (num_char_in_codepoint != characters_in_codepoint) {
    return false;
  }
  *characters_in_utf16 = checked_len_in_utf16;
  return true;
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
