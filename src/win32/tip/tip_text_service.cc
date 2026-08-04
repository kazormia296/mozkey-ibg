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

#include "win32/tip/tip_text_service.h"

#include <ctffunc.h>
#include <ime.h>
#include <msctf.h>
#include <objbase.h>
#include <wil/com.h>
#include <wil/result_macros.h>
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/log.h"
#include "base/const.h"
#include "base/process.h"
#include "base/update_util.h"
#include "base/win32/com.h"
#include "base/win32/com_implements.h"
#include "base/win32/hresult.h"
#include "base/win32/hresultor.h"
#include "base/win32/win_util.h"
#include "grimodex/client_context.h"
#include "protocol/commands.pb.h"
#include "protocol/renderer_callback_provenance.h"
#include "win32/base/win32_window_util.h"
#include "win32/tip/tip_display_attributes.h"
#include "win32/tip/tip_dll_module.h"
#include "win32/tip/tip_edit_session.h"
#include "win32/tip/tip_edit_session_impl.h"
#include "win32/tip/tip_enum_display_attributes.h"
#include "win32/tip/tip_grimodex_context_util.h"
#include "win32/tip/tip_input_mode_manager.h"
#include "win32/tip/tip_keyevent_handler.h"
#include "win32/tip/tip_lang_bar.h"
#include "win32/tip/tip_lang_bar_callback.h"
#include "win32/tip/tip_preferred_touch_keyboard.h"
#include "win32/tip/tip_private_context.h"
#include "win32/tip/tip_reconvert_function.h"
#include "win32/tip/tip_resource.h"
#include "win32/tip/tip_status.h"
#include "win32/tip/tip_thread_context.h"
#include "win32/tip/tip_ui_handler.h"
#include "win32/tip/tip_ui_element_manager.h"

namespace mozc {
namespace win32 {

template <>
bool IsIIDOf<ITfTextInputProcessorEx>(REFIID riid) {
  return IsIIDOf<ITfTextInputProcessorEx, ITfTextInputProcessor>(riid);
}

// Do not respond to QueryInterface calls for internal interfaces and for now.
// TODO(yuryu): Give them a UUID or stop deriving from IUnknown.
template <>
bool IsIIDOf<tsf::TipTextService>(REFIID riid) {
  return false;
}

template <>
bool IsIIDOf<tsf::TipLangBarCallback>(REFIID riid) {
  return false;
}

namespace tsf {
namespace {

class ScopedCallbackSuspension final {
 public:
  explicit ScopedCallbackSuspension(int* depth) : depth_(depth) {
    ++*depth_;
  }
  ScopedCallbackSuspension(const ScopedCallbackSuspension&) = delete;
  ScopedCallbackSuspension& operator=(const ScopedCallbackSuspension&) =
      delete;
  ~ScopedCallbackSuspension() { Resume(); }

  void Resume() {
    if (depth_ != nullptr) {
      --*depth_;
      depth_ = nullptr;
    }
  }

 private:
  int* depth_;
};

// Represents the module handle of this module.
volatile HMODULE g_module = nullptr;

// True if the DLL received DLL_PROCESS_DETACH notification.
volatile bool g_module_unloaded = false;

// Thread Local Storage (TLS) index to specify the current UI thread is
// initialized or not. if ::GetTlsValue(g_tls_index) returns non-zero
// value, the current thread is initialized.
volatile DWORD g_tls_index = TLS_OUT_OF_INDEXES;

constexpr UINT kUpdateUIMessage = WM_USER;
constexpr UINT_PTR kDelayedSessionCommandTimerId = 1;

bool NeedsRendererUpdateOnLayoutChange(TipTextService* text_service,
                                       ITfContext* context) {
  if (text_service == nullptr || context == nullptr) {
    return false;
  }

  const std::shared_ptr<TipPrivateContext> private_context =
      text_service->GetPrivateContext(context);
  if (private_context == nullptr) {
    return false;
  }

  const std::shared_ptr<TipThreadContext> thread_context =
      text_service->GetThreadContextLease();
  if (!thread_context) {
    return false;
  }
  const TsfFocusSnapshot renderer_domain =
      CaptureTsfFocusSnapshot(thread_context.get());
  if (!IsTsfContextFocusedForSnapshot(text_service, context, renderer_domain,
                                      /*require_nonsecure=*/true)) {
    return false;
  }
  if (!private_context->IsLastOutputForFocusDomain(
          renderer_domain.focus_epoch, renderer_domain.focus_revision)) {
    // Give the renderer a chance to hide any retained view, but never inspect
    // stale output to decide its geometry.
    return true;
  }

  const commands::Output& output = private_context->last_output();

  if (output.live_conversion() && output.has_preedit()) {
    return true;
  }

  if (output.has_candidate_window() &&
      output.candidate_window().has_category()) {
    return true;
  }

  const TipInputModeManager* input_mode_manager =
      thread_context->GetInputModeManager();
  if (input_mode_manager == nullptr) {
    return false;
  }

  return private_context->input_behavior().use_mode_indicator &&
         input_mode_manager->IsIndicatorVisible();
}

#ifdef GOOGLE_JAPANESE_INPUT_BUILD

constexpr char kHelpUrl[] = "http://www.google.com/support/ime/japanese";
constexpr wchar_t kTaskWindowClassName[] =
    L"Google Japanese Input Task Message Window";

// {67526BED-E4BE-47CA-97F8-3C84D5B408DA}
constexpr GUID kTipPreservedKey_Kanji = {
    0x67526bed,
    0xe4be,
    0x47ca,
    {0x97, 0xf8, 0x3c, 0x84, 0xd5, 0xb4, 0x08, 0xda}};

// {B62565AA-288A-432B-B517-EC333E0F99F3}
constexpr GUID kTipPreservedKey_F10 = {
    0xb62565aa,
    0x288a,
    0x432b,
    {0xb5, 0x17, 0xec, 0x33, 0x3e, 0xf, 0x99, 0xf3}};

// {CF6E26FB-1A11-4D81-BD92-52FA852A42EB}
constexpr GUID kTipPreservedKey_Romaji = {
    0xcf6e26fb,
    0x1a11,
    0x4d81,
    {0xbd, 0x92, 0x52, 0xfa, 0x85, 0x2a, 0x42, 0xeb}};

// {EEBABC50-7FEC-4A08-9E1D-0BEF628B5F0E}
constexpr GUID kTipFunctionProvider = {
    0xeebabc50,
    0x7fec,
    0x4a08,
    {0x9e, 0x1d, 0xb, 0xef, 0x62, 0x8b, 0x5f, 0x0e}};

#else  // GOOGLE_JAPANESE_INPUT_BUILD

constexpr char kHelpUrl[] = "https://github.com/kazormia296/mozkey-ibg";
constexpr wchar_t kTaskWindowClassName[] =
    L"Mozkey IbG Immersive Task Message Window";

// {6B866013-769D-4BBA-B2EA-314E82354485}
constexpr GUID kTipPreservedKey_Kanji = {
    0x6b866013,
    0x769d,
    0x4bba,
    {0xb2, 0xea, 0x31, 0x4e, 0x82, 0x35, 0x44, 0x85}};

// {439741D0-9C51-4DFD-8309-1A445836DBA5}
constexpr GUID kTipPreservedKey_F10 = {
    0x439741d0,
    0x9c51,
    0x4dfd,
    {0x83, 0x09, 0x1a, 0x44, 0x58, 0x36, 0xdb, 0xa5}};

// {DA38487F-29C9-45FF-A043-25FB083F2596}
constexpr GUID kTipPreservedKey_Romaji = {
    0xda38487f,
    0x29c9,
    0x45ff,
    {0xa0, 0x43, 0x25, 0xfb, 0x08, 0x3f, 0x25, 0x96}};

// {5389807A-F7F6-4983-AF32-8B8851E7A1CF}
constexpr GUID kTipFunctionProvider = {
    0x5389807a,
    0xf7f6,
    0x4983,
    {0xaf, 0x32, 0x8b, 0x88, 0x51, 0xe7, 0xa1, 0xcf}};

#endif  // GOOGLE_JAPANESE_INPUT_BUILD

HRESULT SpawnTool(const std::string& command) {
  if (!Process::SpawnMozcProcess(kMozcTool, "--mode=" + command)) {
    return E_FAIL;
  }
  return S_OK;
}

commands::CompositionMode GetMozcMode(TipLangBarCallback::ItemId menu_id) {
  switch (menu_id) {
    case TipLangBarCallback::kDirect:
      return commands::DIRECT;
    case TipLangBarCallback::kHiragana:
      return commands::HIRAGANA;
    case TipLangBarCallback::kFullKatakana:
      return commands::FULL_KATAKANA;
    case TipLangBarCallback::kHalfAlphanumeric:
      return commands::HALF_ASCII;
    case TipLangBarCallback::kFullAlphanumeric:
      return commands::FULL_ASCII;
    case TipLangBarCallback::kHalfKatakana:
      return commands::HALF_KATAKANA;
    default:
      DLOG(FATAL) << "Unexpected item id: " << menu_id;
      // Fall back to DIRECT in release builds.
      return commands::DIRECT;
  }
}

std::string GetMozcToolCommand(TipLangBarCallback::ItemId menu_id) {
  switch (menu_id) {
    case TipLangBarCallback::kProperty:
      // Open the config dialog.
      return "config_dialog";
    case TipLangBarCallback::kDictionary:
      // Open the dictionary tool.
      return "dictionary_tool";
    case TipLangBarCallback::kWordRegister:
      // Open the word register dialog.
      return "word_register_dialog";
    case TipLangBarCallback::kAbout:
      // Open the about dialog.
      return "about_dialog";
    default:
      DLOG(FATAL) << "Unexpected item id: " << menu_id;
      return "";
  }
}

void EnsureKanaLockUnlocked() {
  // Clear Kana-lock state so that users can input their passwords.
  BYTE keyboard_state[256];
  ::GetKeyboardState(keyboard_state);
  keyboard_state[VK_KANA] = 0;
  ::SetKeyboardState(keyboard_state);
}

// A COM-independent way to instantiate Category Manager object.
wil::com_ptr_nothrow<ITfCategoryMgr> GetCategoryMgr() {
  wil::com_ptr_nothrow<ITfCategoryMgr> ptr;
  return SUCCEEDED(TF_CreateCategoryMgr(&ptr)) ? ptr : nullptr;
}

// Custom hash function for wil::com_ptr_nothrow.
template <typename T>
struct ComPtrHash {
  size_t operator()(const wil::com_ptr_nothrow<T>& value) const {
    // The minimum size of COM objects is the pointer to vtable.
    // For instance the last 3 bits are guaranteed to be zero on 64-bit
    // processes.
    constexpr size_t kUnusedBits =
        std::max(std::bit_width(sizeof(void*)), 1) - 1;
    // Compress the data by shifting unused bits.
    return reinterpret_cast<size_t>(value.get()) >> kUnusedBits;
  }
};

// Custom hash function for GUID.
struct GuidHash {
  size_t operator()(const GUID& value) const {
    // Compress the data by shifting unused bits.
    return value.Data1;
  }
};

// Wraps |TipPrivateContext| with a sink cleanup callback.
class PrivateContextWrapper final {
 public:
  PrivateContextWrapper() = default;
  PrivateContextWrapper(const PrivateContextWrapper&) = delete;
  PrivateContextWrapper& operator=(const PrivateContextWrapper&) = delete;

  ~PrivateContextWrapper() { Retire(); }

  TipPrivateContext* get() { return &private_context_; }

  void AddSinkCleaner(absl::AnyInvocable<void() &&> sink_cleaner) {
    if (retired_) {
      std::move(sink_cleaner)();
      return;
    }
    sink_cleaners_.push_back(std::move(sink_cleaner));
  }

  void Retire() {
    if (retired_) {
      return;
    }
    retired_ = true;
    std::vector<absl::AnyInvocable<void() &&>> sink_cleaners =
        std::move(sink_cleaners_);
    for (auto& sink_cleaner : sink_cleaners) {
      std::move(sink_cleaner)();
    }
  }

 private:
  bool retired_ = false;
  std::vector<absl::AnyInvocable<void() &&>> sink_cleaners_;
  TipPrivateContext private_context_;
};

// An observer that binds ITfCompositionSink::OnCompositionTerminated callback
// to TipEditSession::OnCompositionTerminated.
class CompositionSinkImpl final : public TipComImplements<ITfCompositionSink> {
 public:
  CompositionSinkImpl(wil::com_ptr_nothrow<TipTextService> text_service,
                      wil::com_ptr_nothrow<ITfContext> context,
                      TsfFocusSnapshot composition_domain,
                      uint64_t composition_generation)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        composition_domain_(composition_domain),
        composition_generation_(composition_generation) {}

  // Implements the ITfCompositionSink::OnCompositionTerminated() function.
  // This function is called by Windows when an ongoing composition is
  // terminated by applications.
  STDMETHODIMP OnCompositionTerminated(TfEditCookie write_cookie,
                                       ITfComposition* composition) override {
    return TipEditSessionImpl::OnCompositionTerminated(
        text_service_.get(), context_.get(), composition, write_cookie,
        composition_domain_.focus_epoch,
        composition_domain_.focus_revision, composition_generation_);
  }

 private:
  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  const TsfFocusSnapshot composition_domain_;
  const uint64_t composition_generation_;
};

// Represents preserved keys used by this class.
constexpr wchar_t kTipKeyTilde[] = L"OnOff";
constexpr wchar_t kTipKeyKanji[] = L"Kanji";
constexpr wchar_t kTipKeyF10[] = L"Function 10";
constexpr wchar_t kTipKeyRoman[] = L"Roman";
constexpr wchar_t kTipKeyNoRoman[] = L"NoRoman";

struct PreserveKeyItem {
  const GUID& guid;
  TF_PRESERVEDKEY key;
  DWORD mapped_vkey;
  const wchar_t* description;
  size_t length;
};

constexpr PreserveKeyItem kPreservedKeyItems[] = {
    {kTipPreservedKey_Kanji,
     {VK_OEM_3, TF_MOD_ALT},
     VK_OEM_3,
     &kTipKeyTilde[0],
     std::size(kTipKeyTilde) - 1},
    {kTipPreservedKey_Kanji,
     {VK_KANJI, TF_MOD_IGNORE_ALL_MODIFIER},
     // KeyEventHandler maps VK_KANJI to KeyEvent::NO_SPECIALKEY instead of
     // KeyEvent::KANJI because of an anomaly of IMM32 behavior. So, in TSF
     // mode, we treat VK_KANJI as if it was VK_DBE_DBCSCHAR. See b/7592743 and
     // b/7970379 about what happened.
     VK_DBE_DBCSCHAR,
     &kTipKeyKanji[0],
     std::size(kTipKeyKanji) - 1},
    {kTipPreservedKey_Romaji,
     {VK_DBE_ROMAN, TF_MOD_IGNORE_ALL_MODIFIER},
     VK_DBE_ROMAN,
     &kTipKeyRoman[0],
     std::size(kTipKeyRoman) - 1},
    {kTipPreservedKey_Romaji,
     {VK_DBE_NOROMAN, TF_MOD_IGNORE_ALL_MODIFIER},
     VK_DBE_NOROMAN,
     &kTipKeyNoRoman[0],
     std::size(kTipKeyNoRoman) - 1},
    {kTipPreservedKey_F10,
     {VK_F10, 0},
     VK_F10,
     &kTipKeyF10[0],
     std::size(kTipKeyF10) - 1},
};

class UpdateUiEditSessionImpl final : public TipComImplements<ITfEditSession> {
 public:
  // The ITfEditSession interface method.
  // This function is called back by the TSF thread manager when an edit
  // request is granted.
  STDMETHODIMP DoEditSession(TfEditCookie edit_cookie) override {
    if (!IsTsfContextFocusedForSnapshot(text_service_.get(), context_.get(),
                                        renderer_domain_,
                                        /*require_nonsecure=*/true)) {
      return S_OK;
    }
    TipUiHandler::Update(text_service_.get(), context_.get(), edit_cookie,
                         renderer_domain_.focus_epoch,
                         renderer_domain_.focus_revision);
    return S_OK;
  }

  static bool BeginRequest(TipTextService* text_service, ITfContext* context) {
    // When RequestEditSession fails, it does not maintain the reference count.
    // So we need to ensure that AddRef/Release should be called at least once
    // per object.
    const TsfFocusSnapshot renderer_domain =
        CaptureTsfFocusSnapshot(text_service->GetThreadContext());
    if (!IsTsfContextFocusedForSnapshot(text_service, context,
                                        renderer_domain,
                                        /*require_nonsecure=*/true)) {
      return false;
    }
    wil::com_ptr_nothrow<ITfEditSession> edit_session(
        new UpdateUiEditSessionImpl(text_service, context, renderer_domain));

    HRESULT edit_session_result = S_OK;
    const HRESULT result = context->RequestEditSession(
        text_service->GetClientID(), edit_session.get(),
        TF_ES_ASYNCDONTCARE | TF_ES_READ, &edit_session_result);
    return SUCCEEDED(result) && SUCCEEDED(edit_session_result);
  }

 private:
  UpdateUiEditSessionImpl(wil::com_ptr_nothrow<TipTextService> text_service,
                          wil::com_ptr_nothrow<ITfContext> context,
                          TsfFocusSnapshot renderer_domain)
      : text_service_(std::move(text_service)),
        context_(std::move(context)),
        renderer_domain_(renderer_domain) {}

  wil::com_ptr_nothrow<TipTextService> text_service_;
  wil::com_ptr_nothrow<ITfContext> context_;
  TsfFocusSnapshot renderer_domain_;
};

bool RegisterWindowClass(HINSTANCE module_handle, const wchar_t* class_name,
                         WNDPROC window_procedure) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = window_procedure;
  wc.hInstance = module_handle;
  wc.lpszClassName = class_name;

  const ATOM atom = ::RegisterClassExW(&wc);
  return atom != INVALID_ATOM;
}

class TipTextServiceImpl
    : public TipComImplements<
          ITfTextInputProcessorEx, ITfDisplayAttributeProvider,
          ITfThreadMgrEventSink, ITfThreadFocusSink, ITfTextEditSink,
          ITfTextLayoutSink, ITfKeyEventSink, ITfFnConfigure,
          ITfFunctionProvider, ITfCompartmentEventSink, TipLangBarCallback,
          TipTextService> {
 public:
  TipTextServiceImpl()
      : client_id_(TF_CLIENTID_NULL),
        activate_flags_(0),
        thread_mgr_cookie_(TF_INVALID_COOKIE),
        thread_focus_cookie_(TF_INVALID_COOKIE),
        keyboard_openclose_cookie_(TF_INVALID_COOKIE),
        keyboard_inputmode_conversion_cookie_(TF_INVALID_COOKIE),
        input_attribute_(TF_INVALID_GUIDATOM),
        converted_attribute_(TF_INVALID_GUIDATOM),
        thread_context_(nullptr),
        task_window_handle_(nullptr),
        renderer_callback_window_handle_(nullptr),
        has_pending_delayed_session_command_(false) {}

  static bool OnDllProcessAttach(HMODULE module_handle) {
    if (!RegisterWindowClass(module_handle, kTaskWindowClassName,
                             TaskWindowProc)) {
      return false;
    }

    if (!RegisterWindowClass(module_handle, kMessageReceiverClassName,
                             RendererCallbackWidnowProc)) {
      return false;
    }
    return true;
  }

  static void OnDllProcessDetach(HMODULE module_handle) {
    ::UnregisterClass(kTaskWindowClassName, module_handle);
    ::UnregisterClass(kMessageReceiverClassName, module_handle);
  }

  // ITfTextInputProcessorEx
  STDMETHODIMP Activate(ITfThreadMgr* thread_mgr,
                        TfClientId client_id) override {
    return ActivateEx(thread_mgr, client_id, 0);
  }
  STDMETHODIMP Deactivate() override {
    if (TipDllModule::IsUnloaded()) {
      // Crash report indicates that this method is called after the DLL is
      // unloaded. In such case, we can do nothing safely.
      return S_OK;
    }
    if (activating_) {
      // Initialization calls into TSF repeatedly.  A nested Deactivate must not
      // tear down a resource whose cookie/handle has not yet been published by
      // the returning Init* call.  Defer teardown until the outer transaction
      // has either published every resource or failed.
      activation_cancel_requested_ = true;
      activation_active_ = false;
      private_contexts_accepting_ = false;
      renderer_callback_token_ = 0;
      renderer_callback_focus_epoch_ = 0;
      renderer_callback_focus_revision_ = 0;
      renderer_callback_output_generation_ = 0;
      if (thread_context_) {
        thread_context_->GetInputModeManager()->OnInputScopeUnresolved();
      }
      return S_OK;
    }
    if (deactivating_) {
      return S_OK;
    }
    deactivating_ = true;

    activation_active_ = false;
    private_contexts_accepting_ = false;
    active_private_context_.reset();
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    EndAllUiElements();

    // Stop advising the ITfThreadFocusSink events.
    UninitThreadFocusSink();

    // Unregister the hot keys.
    UninitPreservedKey();

    // Stop advising the ITfCompartmentEventSink events.
    UninitCompartmentEventSink();

    // Stop advising the ITfKeyEvent events.
    UninitKeyEventSink();

    // Remove our button menus from the language bar.
    UninitLanguageBar();

    // Stop advising the ITfFunctionProvider events.
    UninitFunctionProvider();

    // Stop advising the ITfThreadMgrEventSink events.
    UninitThreadManagerEventSink();

    UninitPrivateContexts();

    UninitRendererCallbackWindow();

    UninitTaskWindow();

    // Release the ITfCategoryMgr.
    category_.reset();

    // Release the client ID who communicates with this IME.
    client_id_ = TF_CLIENTID_NULL;

    // Release the ITfThreadMgr object who owns this object.
    thread_mgr_.reset();

    TipUiHandler::OnDeactivate(this);

    if (thread_context_ != nullptr) {
      next_activation_focus_epoch_ = grimodex::AdvanceFocusEpoch(
          thread_context_->GetGrimodexFocusEpoch());
    }
    thread_context_.reset();
    StorePointerForCurrentThread(nullptr);
    deactivating_ = false;

    return S_OK;
  }
  STDMETHODIMP ActivateEx(ITfThreadMgr* thread_mgr, TfClientId client_id,
                          DWORD flags) override {
    if (TipDllModule::IsUnloaded()) {
      // Crash report indicates that this method is called after the DLL is
      // unloaded. In such case, we can do nothing safely. b/7915484.
      return S_OK;  // the returned value will be ignored according to the MSDN.
    }
    if (deactivating_) {
      return E_UNEXPECTED;
    }
    HRESULT result = E_UNEXPECTED;

    EnsureKanaLockUnlocked();

    // A stack trace reported in http://b/2243760 implies that a
    // call of DestroyWindow API during the Deactivation may invokes another
    // message dispatch, which, in turn, may cause a problematic reentrant
    // activation.
    // There are potential code paths to cause such a reentrance so we
    // return E_UNEXPECTED if |thread_| has been initialized.
    // TODO(yukawa): Fix this problem.
    if (thread_mgr_ != nullptr) {
      LOG(ERROR) << "Recursive Activation found.";
      return E_UNEXPECTED;
    }
    // Copy the given thread manager.
    thread_mgr_ = thread_mgr;
    if (!thread_mgr_) {
      LOG(ERROR) << "Failed to retrieve ITfThreadMgr interface.";
      return E_UNEXPECTED;
    }
    thread_context_ =
        std::make_shared<TipThreadContext>(next_activation_focus_epoch_);
    thread_has_focus_ = false;
    thread_focus_callback_generation_ = 0;
    active_private_context_.reset();
    pending_context_resync_ = false;
    // Fail closed before publishing the activation or advising any callback.
    // The initial field's InputScope has not been resolved yet, so it must be
    // treated as password input until OnDocumentMgrChanged proves otherwise.
    thread_context_->GetInputModeManager()->OnInputScopeUnresolved();
    StorePointerForCurrentThread(this);
    activation_active_ = false;
    private_contexts_accepting_ = false;

    // Copy the given client ID.
    // An IME can identify an application with this ID.
    client_id_ = client_id;

    // Copy the given activation flags.
    activate_flags_ = flags;

    activating_ = true;
    activation_cancel_requested_ = false;
    const auto abort_activation = [&]() -> HRESULT {
      activating_ = false;
      activation_cancel_requested_ = false;
      return Deactivate();
    };
    const auto init_failed_or_cancelled = [&](HRESULT init_result) {
      return FAILED(init_result) || activation_cancel_requested_;
    };

    result = InitTaskWindow();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitTaskWindow failed: " << result;
      return abort_activation();
    }

    // Do nothing even when we fail to initialize the renderer callback
    // because 1), it is not so critical, and 2) it actually fails in
    // Internet Explorer 10 on Windows 8.
    InitRendererCallbackWindow();
    if (activation_cancel_requested_) {
      return abort_activation();
    }

    // Start advising thread events to this object.
    result = InitThreadManagerEventSink();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitThreadManagerEventSink failed: " << result;
      return abort_activation();
    }

    // Start advising function provider events to this object.
    result = InitFunctionProvider();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitFunctionProvider failed: " << result;
      return abort_activation();
    }

    category_ = GetCategoryMgr();
    if (!category_ || activation_cancel_requested_) {
      LOG(ERROR) << "GetCategoryMgr failed";
      return abort_activation();
    }

    result = InitLanguageBar();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitLanguageBar failed: " << result;
      return abort_activation();
    }

    // Start advising the keyboard events (ITfKeyEvent) to this object.
    result = InitKeyEventSink();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitKeyEventSink failed: " << result;
      return abort_activation();
    }

    // Start advising ITfCompartmentEventSink to this object.
    result = InitCompartmentEventSink();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitCompartmentEventSink failed: " << result;
      return abort_activation();
    }

    // Register the hot-keys used by this object to Windows.
    result = InitPreservedKey();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitPreservedKey failed: " << result;
      return abort_activation();
    }

    // Start advising ITfThreadFocusSink to this object.
    result = InitThreadFocusSink();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitThreadFocusSink failed: " << result;
      return abort_activation();
    }
    // ITfSource::AdviseSink does not replay the current thread-focus state.
    // Query it after the sink is installed so an activation on an already
    // focused thread is not left permanently fail-closed.  A callback that
    // reenters this query is newer and therefore wins over the returned value.
    {
      const uint64_t callback_generation =
          thread_focus_callback_generation_;
      BOOL initial_thread_focus = FALSE;
      result = thread_mgr_->IsThreadFocus(&initial_thread_focus);
      if (init_failed_or_cancelled(result)) {
        LOG(ERROR) << "IsThreadFocus failed: " << result;
        return abort_activation();
      }
      if (thread_focus_callback_generation_ == callback_generation) {
        thread_has_focus_ = initial_thread_focus != FALSE;
      }
    }

    // Initialize text attributes used by this object.
    result = InitDisplayAttributes();
    if (init_failed_or_cancelled(result)) {
      LOG(ERROR) << "InitDisplayAttributes failed: " << result;
      return abort_activation();
    }

    activating_ = false;
    if (activation_cancel_requested_) {
      activation_cancel_requested_ = false;
      return Deactivate();
    }
    activation_active_ = true;
    private_contexts_accepting_ = true;

    const std::shared_ptr<TipThreadContext> activation_thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> activation_thread_manager = thread_mgr_;
    if (!activation_thread_context || !activation_thread_manager) {
      return S_OK;
    }

    // Write a registry value for usage tracking by Omaha.
    // We ignore the returned value by the function because we should not
    // disturb the application by the result of this function.
    if (!mozc::UpdateUtil::WriteActiveUsageInfo()) {
      LOG(WARNING) << "WriteActiveUsageInfo failed";
    }
    if (!IsCurrentActivation(activation_thread_context)) {
      return S_OK;
    }

    // Copy the initial mode.
    DWORD native_mode = 0;
    if (TipStatus::GetInputModeConversion(activation_thread_manager.get(),
                                          client_id,
                                          &native_mode)) {
      if (!IsCurrentActivation(activation_thread_context)) {
        return S_OK;
      }
      const bool open = TipStatus::IsOpen(activation_thread_manager.get());
      if (!IsCurrentActivation(activation_thread_context)) {
        return S_OK;
      }
      activation_thread_context->GetInputModeManager()->OnInitialize(open,
                                                                     native_mode);
    }

    // Emulate document changed event against the current document manager.
    {
      wil::com_ptr_nothrow<ITfDocumentMgr> document_mgr;
      result = activation_thread_manager->GetFocus(&document_mgr);
      if (FAILED(result)) {
        return IsCurrentActivation(activation_thread_context) ? Deactivate()
                                                              : S_OK;
      }
      if (!IsCurrentActivation(activation_thread_context)) {
        return S_OK;
      }
      if (document_mgr != nullptr) {
        wil::com_ptr_nothrow<ITfContext> context;
        result = document_mgr->GetBase(&context);
        if (SUCCEEDED(result) &&
            IsCurrentActivation(activation_thread_context)) {
          EnsurePrivateContextExists(context.get());
        }
      }
      if (!IsCurrentActivation(activation_thread_context)) {
        return S_OK;
      }

      const TsfFocusSnapshot bootstrap_domain =
          CaptureTsfFocusSnapshot(activation_thread_context.get());
      result = OnDocumentMgrChanged(document_mgr.get(), bootstrap_domain);
      if (FAILED(result)) {
        return Deactivate();
      }
    }

    return result;
  }

  // ITfDisplayAttributeProvider
  STDMETHODIMP
  EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** attributes) override {
    if (attributes == nullptr) {
      return E_INVALIDARG;
    }

    *attributes = MakeComPtr<TipEnumDisplayAttributes>().detach();
    return S_OK;
  }
  STDMETHODIMP GetDisplayAttributeInfo(
      REFGUID guid, ITfDisplayAttributeInfo** attribute) override {
    if (attribute == nullptr) {
      return E_INVALIDARG;
    }

    // Compare the given GUID with known ones and creates a new instance of the
    // specified display attribute.
    if (::IsEqualGUID(guid, TipDisplayAttributeInput::guid())) {
      *attribute = MakeComPtr<TipDisplayAttributeInput>().detach();
    } else if (::IsEqualGUID(guid, TipDisplayAttributeConverted::guid())) {
      *attribute = MakeComPtr<TipDisplayAttributeConverted>().detach();
    } else {
      *attribute = nullptr;
      return E_INVALIDARG;
    }

    return S_OK;
  }

  // ITfThreadMgrEventSink
  STDMETHODIMP
  OnInitDocumentMgr(ITfDocumentMgr* document) override {
    // In order to defer the initialization timing of TipPrivateContext,
    // we won't call OnDocumentMgrChanged against |document| here.
    return S_OK;
  }
  STDMETHODIMP
  OnUninitDocumentMgr(ITfDocumentMgr* document) override {
    // Usually |document| no longer has any context here: all the contexts are
    // likely to be destroyed through ITfThreadMgrEventSink::OnPushContext.
    // We enumerate remaining contexts just in case.

    if (!document) {
      return E_INVALIDARG;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    if (!thread_context) {
      return S_OK;
    }

    wil::com_ptr_nothrow<IEnumTfContexts> enum_context;
    RETURN_IF_FAILED_HRESULT(document->EnumContexts(&enum_context));
    if (!IsCurrentActivation(thread_context)) {
      return S_OK;
    }
    while (true) {
      wil::com_ptr_nothrow<ITfContext> context;
      ULONG fetched = 0;
      const HRESULT hr = enum_context->Next(1, &context, &fetched);
      if (FAILED(hr)) {
        return hr;
      }
      if (hr == S_FALSE || fetched == 0) {
        break;
      }
      if (!IsCurrentActivation(thread_context)) {
        return S_OK;
      }
      RemovePrivateContextIfExists(context.get());
      if (!IsCurrentActivation(thread_context)) {
        return S_OK;
      }
    }

    return S_OK;
  }
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* focused,
                          ITfDocumentMgr* /*previous*/) override {
    if (callback_suspension_depth_ > 0) {
      RecordSuspendedContextTransition();
      return S_OK;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return S_OK;
    }
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    thread_context->IncrementFocusRevision();
    const TsfFocusSnapshot transition_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    const bool expected_thread_focus = thread_has_focus_;
    // This must precede the previous-domain IPC below because that call can
    // pump messages and expose the newly focused context reentrantly.
    thread_context->GetInputModeManager()->OnInputScopeUnresolved();
    TipUiHandler::OnFocusChange(this, nullptr, transition_domain.focus_epoch,
                                transition_domain.focus_revision);
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    if (!expected_thread_focus) {
      return S_OK;
    }

    // Detach the retained active client before the reentrant IPC.  The
    // inactive-client RESET deliberately leaves the shared domain tracker
    // unchanged; the new focused context will install its own domain below.
    const std::shared_ptr<TipPrivateContext> previous_private_context =
        std::exchange(active_private_context_, nullptr);
    if (previous_private_context != nullptr &&
        !SendTsfResetContextForInactiveClient(
            this, previous_private_context->GetClient())) {
      LOG(WARNING) << "Failed to revoke the previous TSF input domain";
    }
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    return OnDocumentMgrChanged(focused, transition_domain);
  }
  STDMETHODIMP OnPushContext(ITfContext* context) override {
    if (callback_suspension_depth_ > 0) {
      QueueSuspendedContextLifecycleEvent(/*is_push=*/true, context);
      return S_OK;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return S_OK;
    }
    ScopedCallbackSuspension callback_suspension(
        &callback_suspension_depth_);
    EnsurePrivateContextExists(context);
    if (!IsCurrentActivation(thread_context)) {
      return S_OK;
    }
    const bool focused_stack = ShouldTreatContextStackAsFocused(
        context, thread_manager.get(), thread_context);
    if (!IsCurrentActivation(thread_context)) {
      return S_OK;
    }
    if (!focused_stack) {
      // TSF reports stack changes for background document managers too.  They
      // must not revoke the renderer or advance the active input domain.
      callback_suspension.Resume();
      DrainPendingContextLifecycleEvents();
      ResyncCurrentFocusAfterSuspension(thread_context, thread_manager.get());
      return S_OK;
    }
    if (pending_context_resync_) {
      callback_suspension.Resume();
      DrainPendingContextLifecycleEvents();
      ResyncCurrentFocusAfterSuspension(thread_context, thread_manager.get());
      return S_OK;
    }
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    thread_context->IncrementFocusRevision();
    const TsfFocusSnapshot transition_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    const bool expected_thread_focus = thread_has_focus_;
    thread_context->GetInputModeManager()->OnInputScopeUnresolved();
    callback_suspension.Resume();
    DrainPendingContextLifecycleEvents();
    if (pending_context_resync_ ||
        !IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      ResyncCurrentFocusAfterSuspension(thread_context, thread_manager.get());
      return S_OK;
    }
    TipUiHandler::OnFocusChange(this, nullptr, transition_domain.focus_epoch,
                                transition_domain.focus_revision);
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    const std::shared_ptr<TipPrivateContext> previous_private_context =
        std::exchange(active_private_context_, nullptr);
    if (previous_private_context != nullptr &&
        !SendTsfResetContextForInactiveClient(
            this, previous_private_context->GetClient())) {
      LOG(WARNING) << "Failed to revoke pushed TSF context";
    }
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    if (!expected_thread_focus) {
      return S_OK;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> focused_document;
    if (FAILED(thread_manager->GetFocus(&focused_document)) ||
        !IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    return OnDocumentMgrChanged(focused_document.get(), transition_domain);
  }
  STDMETHODIMP OnPopContext(ITfContext* context) override {
    if (callback_suspension_depth_ > 0) {
      QueueSuspendedContextLifecycleEvent(/*is_push=*/false, context);
      return S_OK;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return S_OK;
    }
    ScopedCallbackSuspension callback_suspension(
        &callback_suspension_depth_);
    const bool focused_stack = ShouldTreatContextStackAsFocused(
        context, thread_manager.get(), thread_context);
    if (!IsCurrentActivation(thread_context)) {
      return S_OK;
    }
    if (!focused_stack) {
      // Retire only the popped background client's state.  The focused
      // document's epoch, secure-input state, and candidate view are unrelated
      // to this stack event.
      const std::shared_ptr<PrivateContextWrapper> retired_wrapper =
          ExtractPrivateContextIfExists(context);
      const std::shared_ptr<TipPrivateContext> private_context =
          retired_wrapper
              ? std::shared_ptr<TipPrivateContext>(retired_wrapper,
                                                   retired_wrapper->get())
              : nullptr;
      callback_suspension.Resume();
      DrainPendingContextLifecycleEvents();
      if (private_context != nullptr &&
          !SendTsfResetContextForInactiveClient(
              this, private_context->GetClient())) {
        LOG(WARNING) << "Failed to revoke popped background TSF context";
      }
      if (!IsCurrentActivation(thread_context)) {
        return S_OK;
      }
      ResyncCurrentFocusAfterSuspension(thread_context, thread_manager.get());
      return S_OK;
    }
    if (pending_context_resync_) {
      ExtractPrivateContextIfExists(context);
      callback_suspension.Resume();
      DrainPendingContextLifecycleEvents();
      ResyncCurrentFocusAfterSuspension(thread_context, thread_manager.get());
      return S_OK;
    }
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    thread_context->IncrementFocusRevision();
    const TsfFocusSnapshot transition_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    const bool expected_thread_focus = thread_has_focus_;
    thread_context->GetInputModeManager()->OnInputScopeUnresolved();
    const std::shared_ptr<PrivateContextWrapper> retired_wrapper =
        ExtractPrivateContextIfExists(context);
    const std::shared_ptr<TipPrivateContext> private_context =
        retired_wrapper
            ? std::shared_ptr<TipPrivateContext>(retired_wrapper,
                                                 retired_wrapper->get())
            : nullptr;
    callback_suspension.Resume();
    DrainPendingContextLifecycleEvents();
    if (pending_context_resync_ ||
        !IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      ResyncCurrentFocusAfterSuspension(thread_context, thread_manager.get());
      return S_OK;
    }
    TipUiHandler::OnFocusChange(this, nullptr, transition_domain.focus_epoch,
                                transition_domain.focus_revision);
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    const std::shared_ptr<TipPrivateContext> previous_private_context =
        std::exchange(active_private_context_, nullptr);
    const std::shared_ptr<TipPrivateContext> context_to_revoke =
        previous_private_context != nullptr ? previous_private_context
                                            : private_context;
    if (context_to_revoke != nullptr &&
        !SendTsfResetContextForInactiveClient(
            this, context_to_revoke->GetClient())) {
      LOG(WARNING) << "Failed to revoke popped TSF context";
    }
    if (previous_private_context != nullptr && private_context != nullptr &&
        previous_private_context.get() != private_context.get() &&
        !SendTsfResetContextForInactiveClient(
            this, private_context->GetClient())) {
      LOG(WARNING) << "Failed to revoke the distinct popped TSF context";
    }
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    if (!expected_thread_focus) {
      return S_OK;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> focused_document;
    if (FAILED(thread_manager->GetFocus(&focused_document)) ||
        !IsFocusTransitionCurrent(thread_context, transition_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    return OnDocumentMgrChanged(focused_document.get(), transition_domain);
  }

  // ITfThreadFocusSink
  STDMETHODIMP OnSetThreadFocus() override {
    ++thread_focus_callback_generation_;
    thread_has_focus_ = true;
    if (callback_suspension_depth_ > 0) {
      RecordSuspendedContextTransition();
      return S_OK;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return S_OK;
    }
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    thread_context->IncrementFocusRevision();
    const TsfFocusSnapshot transition_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    thread_context->GetInputModeManager()->OnInputScopeUnresolved();
    TipUiHandler::OnFocusChange(this, nullptr, transition_domain.focus_epoch,
                                transition_domain.focus_revision);
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  /*expected_thread_focus=*/true)) {
      return S_OK;
    }
    EnsureKanaLockUnlocked();
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  /*expected_thread_focus=*/true)) {
      return S_OK;
    }

    // A temporary workaround for b/24793812.  When previous attempt to
    // establish conection failed, retry again as if this was the first attempt.
    // TODO(yukawa): We should give up if this fails a number of times.
    if (WinUtil::IsProcessSandboxed()) {
      const std::shared_ptr<TipPrivateContext> private_context =
          GetFocusedPrivateContext(thread_manager.get(), thread_context);
      if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                    /*expected_thread_focus=*/true)) {
        return S_OK;
      }
      if (private_context != nullptr) {
        private_context->EnsureInitialized();
      }
      if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                    /*expected_thread_focus=*/true)) {
        return S_OK;
      }
    }

    // While ITfThreadMgrEventSink::OnSetFocus notifies the logical focus inside
    // the application, ITfThreadFocusSink notifies the OS-level keyboard focus
    // events. In both cases, Mozc's UI visibility should be updated.
    wil::com_ptr_nothrow<ITfDocumentMgr> document_manager;
    if (FAILED(thread_manager->GetFocus(&document_manager)) ||
        !IsFocusTransitionCurrent(thread_context, transition_domain,
                                  /*expected_thread_focus=*/true)) {
      return S_OK;
    }
    if (!document_manager) {
      return S_OK;
    }
    return OnDocumentMgrChanged(document_manager.get(), transition_domain);
  }
  STDMETHODIMP OnKillThreadFocus() override {
    ++thread_focus_callback_generation_;
    thread_has_focus_ = false;
    if (callback_suspension_depth_ > 0) {
      RecordSuspendedContextTransition();
      return S_OK;
    }
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return S_OK;
    }
    // See the comment in OnSetThreadFocus().
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    thread_context->IncrementFocusRevision();
    const TsfFocusSnapshot transition_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    thread_context->GetInputModeManager()->OnInputScopeUnresolved();
    TipUiHandler::OnFocusChange(this, nullptr, transition_domain.focus_epoch,
                                transition_domain.focus_revision);
    if (!IsFocusTransitionCurrent(thread_context, transition_domain,
                                  /*expected_thread_focus=*/false)) {
      return S_OK;
    }
    const std::shared_ptr<TipPrivateContext> private_context =
        std::exchange(active_private_context_, nullptr);
    if (private_context != nullptr &&
        !SendTsfResetContextForInactiveClient(this,
                                              private_context->GetClient())) {
      LOG(WARNING) << "Failed to revoke TSF context after focus loss";
    }
    return S_OK;
  }

  // ITfTextEditSink
  STDMETHODIMP OnEndEdit(ITfContext* context, TfEditCookie edit_cookie,
                         ITfEditRecord* edit_record) override {
    return TipEditSessionImpl::OnEndEdit(this, context, edit_cookie,
                                         edit_record);
  }

  STDMETHODIMP
  OnLayoutChange(ITfContext* context, TfLayoutCode layout_code,
                 ITfContextView* context_view) override {
    if (!NeedsRendererUpdateOnLayoutChange(this, context)) {
      return S_OK;
    }

    TipEditSession::OnLayoutChangedAsync(this, context);
    return S_OK;
  }

  // ITfKeyEventSink
  STDMETHODIMP OnSetFocus(BOOL foreground) override { return S_OK; }
  STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam,
                             BOOL* eaten) override {
    BOOL dummy_eaten = FALSE;
    if (eaten == nullptr) {
      eaten = &dummy_eaten;
    }
    *eaten = FALSE;
    return TipKeyeventHandler::OnTestKeyDown(this, context, wparam, lparam,
                                             eaten);
  }

  // ITfKeyEventSink
  STDMETHODIMP OnTestKeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam,
                           BOOL* eaten) override {
    BOOL dummy_eaten = FALSE;
    if (eaten == nullptr) {
      eaten = &dummy_eaten;
    }
    *eaten = FALSE;
    return TipKeyeventHandler::OnTestKeyUp(this, context, wparam, lparam,
                                           eaten);
  }
  STDMETHODIMP OnKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam,
                         BOOL* eaten) override {
    BOOL dummy_eaten = FALSE;
    if (eaten == nullptr) {
      eaten = &dummy_eaten;
    }
    *eaten = FALSE;
    return TipKeyeventHandler::OnKeyDown(this, context, wparam, lparam, eaten);
  }
  STDMETHODIMP OnKeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam,
                       BOOL* eaten) override {
    BOOL dummy_eaten = FALSE;
    if (eaten == nullptr) {
      eaten = &dummy_eaten;
    }
    *eaten = FALSE;
    return TipKeyeventHandler::OnKeyUp(this, context, wparam, lparam, eaten);
  }
  STDMETHODIMP OnPreservedKey(ITfContext* context, REFGUID guid,
                              BOOL* eaten) override {
    HRESULT result = S_OK;
    BOOL dummy_eaten = FALSE;
    if (eaten == nullptr) {
      eaten = &dummy_eaten;
    }
    *eaten = FALSE;
    const auto it = preserved_key_map_.find(guid);
    if (it == preserved_key_map_.end()) {
      return result;
    }
    const UINT vk = it->second;
    const UINT alt_down = (::GetKeyState(VK_MENU) & 0x8000) != 0 ? 1 : 0;
    const UINT scan_code = ::MapVirtualKey(VK_F10, 0);
    const UINT lparam = (alt_down << 29) | (scan_code << 16) | 1;
    result = TipKeyeventHandler::OnKeyDown(this, context, vk, lparam, eaten);
    if (*eaten == FALSE && vk == VK_F10) {
      // Special treatment for F10:
      // Setting FALSE to |*eaten| is not enough when F10 key is handled by the
      // application. So here manually compose WM_SYSKEYDOWN message to emulate
      // F10 key.
      // http://msdn.microsoft.com/en-us/library/ms646286.aspx
      ::PostMessage(::GetFocus(), WM_SYSKEYDOWN, VK_F10, lparam);
    }
    return result;
  }

  // ITfFnConfigure
  STDMETHODIMP GetDisplayName(BSTR* name) override {
    if (name == nullptr) {
      return E_INVALIDARG;
    }
    *name = ::SysAllocString(kConfigurationDisplayname);
    return (*name != nullptr) ? S_OK : E_FAIL;
  }
  STDMETHODIMP Show(HWND parent, LANGID langid, REFGUID profile) override {
    return SpawnTool("config_dialog");
  }

  // ITfFunctionProvider
  STDMETHODIMP GetType(GUID* guid) override {
    if (guid == nullptr) {
      return E_INVALIDARG;
    }
    *guid = kTipFunctionProvider;
    return S_OK;
  }
  STDMETHODIMP GetDescription(BSTR* description) override {
    if (description == nullptr) {
      return E_INVALIDARG;
    }
    *description = SysAllocString(L"");
    return *description ? S_OK : E_FAIL;
  }
  STDMETHODIMP GetFunction(const GUID& guid, const IID& iid,
                           IUnknown** unknown) override {
    if (unknown == nullptr) {
      return E_INVALIDARG;
    }
    if (::IsEqualGUID(IID_ITfFnReconversion, iid)) {
      *unknown = MakeComPtr<TipReconvertFunction>(this).detach();
    } else if (::IsEqualGUID(TipPreferredTouchKeyboard::GetIID(), iid)) {
      *unknown = TipPreferredTouchKeyboard::New().detach();
    } else {
      return E_NOINTERFACE;
    }
    return *unknown ? S_OK : E_OUTOFMEMORY;
  }

  // ITfCompartmentEventSink
  STDMETHODIMP OnChange(const GUID& guid) override {
    if (!thread_mgr_) {
      return E_FAIL;
    }

    if (::IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
      TipEditSession::OnModeChangedAsync(
          this, ReserveModeCallback(TipModeCallbackKind::kConversion));
    } else if (::IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
      TipEditSession::OnOpenCloseChangedAsync(
          this, ReserveModeCallback(TipModeCallbackKind::kOpenClose));
    }
    return S_OK;
  }

  // TipLangBarCallback
  STDMETHODIMP OnMenuSelect(ItemId menu_id) override {
    if (!GetThreadContextLease()) {
      return E_FAIL;
    }
    switch (menu_id) {
      case TipLangBarCallback::kDirect:
      case TipLangBarCallback::kHiragana:
      case TipLangBarCallback::kFullKatakana:
      case TipLangBarCallback::kHalfAlphanumeric:
      case TipLangBarCallback::kFullAlphanumeric:
      case TipLangBarCallback::kHalfKatakana: {
        const commands::CompositionMode mozc_mode = GetMozcMode(menu_id);
        return TipEditSession::SwitchInputModeAsync(this, mozc_mode);
      }
      case TipLangBarCallback::kProperty:
      case TipLangBarCallback::kDictionary:
      case TipLangBarCallback::kWordRegister:
      case TipLangBarCallback::kAbout:
        return SpawnTool(GetMozcToolCommand(menu_id));
      case TipLangBarCallback::kHelp:
        // Open the about dialog.
        return Process::OpenBrowser(kHelpUrl) ? S_OK : E_FAIL;
      default:
        return S_OK;
    }
  }
  STDMETHODIMP OnItemClick(const wchar_t* description) override {
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return E_FAIL;
    }
    // Change input mode to be consistent with MSIME 2012 on Windows 8.
    const bool open =
        thread_context->GetInputModeManager()->GetEffectiveOpenClose();
    if (open) {
      return TipStatus::SetIMEOpen(thread_manager.get(), client_id_, false)
                 ? S_OK
                 : E_FAIL;
    }

    // Like MSIME 2012, switch to Hiragana mode when the LangBar button is
    // clicked.
    return TipEditSession::SwitchInputModeAsync(this, commands::HIRAGANA);
  }

  // TipTextService
  TfClientId GetClientID() const override { return client_id_; }
  ITfThreadMgr* GetThreadManager() const override { return thread_mgr_.get(); }
  TfGuidAtom input_attribute() const override { return input_attribute_; }
  TfGuidAtom converted_attribute() const override {
    return converted_attribute_;
  }
  HWND renderer_callback_window_handle() const override {
    return renderer_callback_window_handle_;
  }
  void SetRendererCallbackProvenance(uint64_t token, uint64_t focus_epoch,
                                     int32_t focus_revision,
                                     uint64_t output_generation) override {
    renderer_callback_token_ = token;
    renderer_callback_focus_epoch_ = token == 0 ? 0 : focus_epoch;
    renderer_callback_focus_revision_ = token == 0 ? 0 : focus_revision;
    renderer_callback_output_generation_ =
        token == 0 ? 0 : output_generation;
  }
  void EndAllUiElements() override {
    std::vector<std::shared_ptr<TipPrivateContext>> private_contexts;
    private_contexts.reserve(private_context_map_.size());
    for (const auto& entry : private_context_map_) {
      private_contexts.emplace_back(entry.second, entry.second->get());
    }
    for (const std::shared_ptr<TipPrivateContext>& private_context :
         private_contexts) {
      private_context->GetUiElementManager()->EndAll(this);
    }
  }

  wil::com_ptr_nothrow<ITfCompositionSink> CreateCompositionSink(
      ITfContext* context, uint64_t composition_focus_epoch,
      int32_t composition_focus_revision,
      uint64_t composition_generation) override {
    return MakeComPtr<CompositionSinkImpl>(
        this, context,
        TsfFocusSnapshot{.focus_epoch = composition_focus_epoch,
                         .focus_revision = composition_focus_revision},
        composition_generation);
  }
  std::shared_ptr<TipPrivateContext> GetPrivateContext(
      ITfContext* context) override {
    if (context == nullptr) {
      return nullptr;
    }
    const auto it = private_context_map_.find(context);
    if (it == private_context_map_.end()) {
      return nullptr;
    }
    return std::shared_ptr<TipPrivateContext>(it->second, it->second->get());
  }
  TipThreadContext* GetThreadContext() override {
    return thread_context_.get();
  }
  std::shared_ptr<TipThreadContext> GetThreadContextLease() override {
    return activation_active_ && !activating_ &&
                   callback_suspension_depth_ == 0
               ? thread_context_
               : nullptr;
  }
  bool HasThreadFocus() const override {
    return activation_active_ && thread_has_focus_;
  }
  uint64_t ReserveActiveOutputApplication(
      uint64_t focus_epoch, int32_t focus_revision) override {
    if (!activation_active_ || !thread_has_focus_ ||
        active_private_context_ == nullptr || thread_context_ == nullptr ||
        !IsTsfFocusSnapshotCurrent(
            thread_context_.get(),
            {.focus_epoch = focus_epoch,
             .focus_revision = focus_revision})) {
      return 0;
    }
    return active_private_context_->ReserveOutputApplicationForFocusDomain(
        focus_epoch, focus_revision);
  }
  uint64_t ReserveModeCallback(TipModeCallbackKind kind) override {
    uint64_t& generation =
        kind == TipModeCallbackKind::kConversion
            ? conversion_mode_callback_generation_
            : open_close_mode_callback_generation_;
    ++generation;
    if (generation == 0) {
      ++generation;
    }
    return generation;
  }
  bool IsModeCallbackCurrent(TipModeCallbackKind kind,
                             uint64_t generation) const override {
    const uint64_t current_generation =
        kind == TipModeCallbackKind::kConversion
            ? conversion_mode_callback_generation_
            : open_close_mode_callback_generation_;
    return activation_active_ && generation != 0 &&
           current_generation == generation;
  }
  void PostUIUpdateMessage() override {
    if (!::IsWindow(task_window_handle_)) {
      return;
    }
    PostMessageW(task_window_handle_, kUpdateUIMessage, 0, 0);
  }

  void PostDelayedSessionCommand(
      ITfContext* context,
      const commands::SessionCommand& command,
      uint32_t delay_millisec,
      uint64_t output_application_generation) override {
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    const HWND task_window = task_window_handle_;
    if (!thread_context || context == nullptr ||
        output_application_generation == 0 || !::IsWindow(task_window) ||
        !IsCurrentActivation(thread_context)) {
      return;
    }
    const std::shared_ptr<TipPrivateContext> private_context =
        GetPrivateContext(context);
    const TsfFocusSnapshot delayed_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    if (!private_context ||
        !private_context->IsOutputApplicationForFocusDomain(
            delayed_domain.focus_epoch, delayed_domain.focus_revision,
            output_application_generation)) {
      return;
    }

    pending_delayed_session_context_ = context;
    pending_delayed_session_command_ = command;
    pending_delayed_session_focus_epoch_ =
        thread_context->GetGrimodexFocusEpoch();
    pending_delayed_session_focus_revision_ =
        thread_context->GetFocusRevision();
    // Output-application generations are intentionally short lived.  In
    // particular, the key-up following the key-down that produced this
    // callback reserves the next generation even when it has no output.  Bind
    // the delayed command to the TSF composition instead so that ordinary key
    // transitions do not cancel live conversion while commit/cancel still do.
    pending_delayed_session_composition_generation_ =
        private_context->composition_generation();
    has_pending_delayed_session_command_ = true;

    ::KillTimer(task_window, kDelayedSessionCommandTimerId);
    if (!IsCurrentActivation(thread_context)) {
      return;
    }

    const UINT delay =
        delay_millisec == 0 ? 1 : static_cast<UINT>(delay_millisec);
    ::SetTimer(task_window,
              kDelayedSessionCommandTimerId,
              delay,
              nullptr);
  }

  void UpdateLangbar(bool enabled, uint32_t mozc_mode) override {
    langbar_.UpdateMenu(enabled, mozc_mode);
  }

  bool IsLangbarInitialized() const override {
    return langbar_.IsInitialized();
  }

 private:
  // Following functions are private utilities.
  static void StorePointerForCurrentThread(TipTextServiceImpl* impl) {
    if (g_module_unloaded) {
      return;
    }
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
      return;
    }
    ::TlsSetValue(g_tls_index, impl);
  }
  static TipTextServiceImpl* Self() {
    if (g_module_unloaded) {
      return nullptr;
    }
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
      return nullptr;
    }
    return static_cast<TipTextServiceImpl*>(::TlsGetValue(g_tls_index));
  }

  HRESULT OnDocumentMgrChanged(ITfDocumentMgr* document_mgr,
                               TsfFocusSnapshot expected_domain) {
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    const auto is_expected_domain_current = [&]() {
      return thread_has_focus_ && IsCurrentActivation(thread_context) &&
             IsTsfFocusSnapshotCurrent(thread_context.get(), expected_domain);
    };
    if (!thread_context || !thread_manager ||
        !is_expected_domain_current() ||
        !IsDocumentManagerCurrentlyFocused(document_mgr, thread_manager.get(),
                                           thread_context)) {
      return S_OK;
    }

    wil::com_ptr_nothrow<ITfContext> context;
    if (document_mgr != nullptr) {
      RETURN_IF_FAILED_HRESULT(document_mgr->GetTop(&context));
      if (!is_expected_domain_current() || context == nullptr ||
          !IsContextTopOfFocusedDocument(
              document_mgr, context.get(), thread_manager.get(),
              thread_context)) {
        return S_OK;
      }
      EnsurePrivateContextExists(context.get());
      if (!is_expected_domain_current() ||
          !IsContextTopOfFocusedDocument(
              document_mgr, context.get(), thread_manager.get(),
              thread_context)) {
        return S_OK;
      }
      // Retain the authoritative active client before any provider or server
      // boundary.  Focus loss can then revoke it without relying on a later
      // GetFocus/GetTop query that may already return no logical focus.
      active_private_context_ = GetPrivateContext(context.get());
    }

    // Scope discovery needs an asynchronous read edit session.  Enter the
    // password policy synchronously so no key, renderer callback, or timer can
    // observe the newly focused domain as non-secure in that gap.
    thread_context->GetInputModeManager()->OnInputScopeUnresolved();
    if (!is_expected_domain_current() ||
        !IsDocumentManagerCurrentlyFocused(document_mgr, thread_manager.get(),
                                           thread_context) ||
        (context != nullptr &&
         !IsContextTopOfFocusedDocument(
             document_mgr, context.get(), thread_manager.get(),
             thread_context))) {
      return S_OK;
    }

    TsfFocusSnapshot document_domain = expected_domain;

    // nullptr document is not an error.
    if (context != nullptr) {
      // Publish the fail-closed domain transition immediately.  The async
      // scope resolver will publish a newer non-secure epoch only after an
      // authoritative InputScope read succeeds.
      const std::shared_ptr<TipPrivateContext> private_context =
          GetPrivateContext(context.get());
      TsfFocusSnapshot installed_domain;
      if (private_context != nullptr &&
          !SendTsfResetContext(this, context.get(),
                               private_context->GetClient(),
                               /*force_secure_input=*/true,
                               &installed_domain)) {
        LOG(WARNING)
            << "Failed to publish the newly focused secure TSF domain";
        return S_OK;
      }
      if (private_context == nullptr) {
        return S_OK;
      }
      document_domain = installed_domain;
      expected_domain = installed_domain;
      if (!IsTsfContextFocusedForSnapshot(
              this, context.get(), document_domain,
              /*require_nonsecure=*/false)) {
        return S_OK;
      }
    }
    TipUiHandler::OnDocumentMgrChanged(
        this, document_mgr, document_domain.focus_epoch,
        document_domain.focus_revision);
    const bool document_current =
        context != nullptr
            ? IsTsfContextFocusedForSnapshot(
                  this, context.get(), document_domain,
                  /*require_nonsecure=*/false)
            : IsDocumentManagerCurrentlyFocused(
                  nullptr, thread_manager.get(), thread_context) &&
                  IsTsfFocusSnapshotCurrent(thread_context.get(),
                                            document_domain);
    if (!document_current) {
      return S_OK;
    }
    if (thread_has_focus_ && IsCurrentActivation(thread_context)) {
      TipEditSession::OnSetFocusAsync(
          this, document_mgr, document_domain.focus_epoch,
          document_domain.focus_revision);
    }
    return S_OK;
  }

  bool IsCurrentActivation(
      const std::shared_ptr<TipThreadContext>& thread_context) const {
    return activation_active_ && thread_context && thread_context_ &&
           thread_context.get() == thread_context_.get();
  }

  bool IsFocusTransitionCurrent(
      const std::shared_ptr<TipThreadContext>& thread_context,
      TsfFocusSnapshot transition_domain,
      bool expected_thread_focus) const {
    return IsCurrentActivation(thread_context) &&
           thread_has_focus_ == expected_thread_focus &&
           IsTsfFocusSnapshotCurrent(thread_context.get(), transition_domain);
  }

  void RecordSuspendedContextTransition() {
    pending_context_resync_ = true;
    if (!activation_active_ || !thread_context_) {
      return;
    }
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    thread_context_->IncrementFocusRevision();
    thread_context_->GetInputModeManager()->OnInputScopeUnresolved();
  }

  void QueueSuspendedContextLifecycleEvent(bool is_push, ITfContext* context) {
    pending_context_lifecycle_events_.emplace_back(is_push, context);
    RecordSuspendedContextTransition();
  }

  void DrainPendingContextLifecycleEvents() {
    if (callback_suspension_depth_ > 0) {
      return;
    }
    while (!pending_context_lifecycle_events_.empty()) {
      auto events = std::move(pending_context_lifecycle_events_);
      pending_context_lifecycle_events_.clear();
      for (auto& [is_push, context] : events) {
        if (is_push) {
          OnPushContext(context.get());
        } else {
          OnPopContext(context.get());
        }
      }
    }
  }

  HRESULT ResyncCurrentFocusAfterSuspension(
      const std::shared_ptr<TipThreadContext>& thread_context,
      ITfThreadMgr* thread_manager) {
    if (!std::exchange(pending_context_resync_, false) ||
        !IsCurrentActivation(thread_context) || thread_manager == nullptr) {
      return S_FALSE;
    }
    const TsfFocusSnapshot resync_domain =
        CaptureTsfFocusSnapshot(thread_context.get());
    const bool expected_thread_focus = thread_has_focus_;
    const std::shared_ptr<TipPrivateContext> previous_private_context =
        std::exchange(active_private_context_, nullptr);
    if (previous_private_context != nullptr &&
        !SendTsfResetContextForInactiveClient(
            this, previous_private_context->GetClient())) {
      LOG(WARNING) << "Failed to revoke TSF context during focus resync";
    }
    if (!IsFocusTransitionCurrent(thread_context, resync_domain,
                                  expected_thread_focus)) {
      return S_OK;
    }
    TipUiHandler::OnFocusChange(this, nullptr, resync_domain.focus_epoch,
                                resync_domain.focus_revision);
    if (!IsFocusTransitionCurrent(thread_context, resync_domain,
                                  expected_thread_focus) ||
        !expected_thread_focus) {
      return S_OK;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> focused_document;
    if (FAILED(thread_manager->GetFocus(&focused_document)) ||
        !IsFocusTransitionCurrent(thread_context, resync_domain,
                                  /*expected_thread_focus=*/true)) {
      return S_OK;
    }
    return OnDocumentMgrChanged(focused_document.get(), resync_domain);
  }

  bool IsDocumentManagerCurrentlyFocused(
      ITfDocumentMgr* document_mgr, ITfThreadMgr* thread_manager,
      const std::shared_ptr<TipThreadContext>& thread_context) {
    if (thread_manager == nullptr || !IsCurrentActivation(thread_context)) {
      return false;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> focused_document;
    if (FAILED(thread_manager->GetFocus(&focused_document)) ||
        !IsCurrentActivation(thread_context)) {
      return false;
    }
    if (document_mgr == nullptr || focused_document == nullptr) {
      return document_mgr == nullptr && focused_document == nullptr;
    }
    const auto expected_identity = ComQuery<IUnknown>(document_mgr);
    const auto focused_identity = ComQuery<IUnknown>(focused_document.get());
    return expected_identity && focused_identity &&
           expected_identity.get() == focused_identity.get() &&
           IsCurrentActivation(thread_context);
  }

  bool IsContextTopOfFocusedDocument(
      ITfDocumentMgr* document_mgr, ITfContext* expected_context,
      ITfThreadMgr* thread_manager,
      const std::shared_ptr<TipThreadContext>& thread_context) {
    if (document_mgr == nullptr || expected_context == nullptr ||
        !IsDocumentManagerCurrentlyFocused(document_mgr, thread_manager,
                                           thread_context)) {
      return false;
    }
    wil::com_ptr_nothrow<ITfContext> top_context;
    if (FAILED(document_mgr->GetTop(&top_context)) || top_context == nullptr ||
        !IsCurrentActivation(thread_context)) {
      return false;
    }
    const auto expected_identity = ComQuery<IUnknown>(expected_context);
    const auto top_identity = ComQuery<IUnknown>(top_context.get());
    return expected_identity && top_identity &&
           expected_identity.get() == top_identity.get() &&
           IsDocumentManagerCurrentlyFocused(document_mgr, thread_manager,
                                             thread_context);
  }

  void EnsurePrivateContextExists(ITfContext* context) {
    if (!private_contexts_accepting_ || context == nullptr) {
      // Do not care about nullptr context.
      return;
    }
    if (private_context_map_.contains(context)) {
      return;
    }

    // Publish the wrapper before crossing any COM boundary. A nested TSF
    // callback will then observe the same registry entry instead of advising
    // duplicate sinks or resurrecting an entry that was popped reentrantly.
    const std::shared_ptr<PrivateContextWrapper> wrapper =
        std::make_shared<PrivateContextWrapper>();
    const auto [inserted_it, inserted] =
        private_context_map_.emplace(context, wrapper);
    if (!inserted || inserted_it->second != wrapper) {
      return;
    }

    auto source = ComQuery<ITfSource>(context);
    const auto wrapper_is_registered = [&]() {
      const auto it = private_context_map_.find(context);
      return it != private_context_map_.end() && it->second == wrapper;
    };
    if (!wrapper_is_registered()) {
      return;
    }
    if (!source) {
      // In general this should not happen. Keep the already-published private
      // context without sink-cleanup callbacks.
      return;
    }

    DWORD text_edit_sink_cookie = TF_INVALID_COOKIE;
    DWORD text_layout_sink_cookie = TF_INVALID_COOKIE;
    if (SUCCEEDED(source->AdviseSink(
            IID_ITfTextEditSink,
            absl::implicit_cast<ITfTextEditSink*>(this),
            &text_edit_sink_cookie)) &&
        text_edit_sink_cookie != TF_INVALID_COOKIE) {
      wrapper->AddSinkCleaner(
          [source, text_edit_sink_cookie]() mutable {
            source->UnadviseSink(text_edit_sink_cookie);
          });
    }
    if (!wrapper_is_registered()) {
      return;
    }
    if (SUCCEEDED(source->AdviseSink(
            IID_ITfTextLayoutSink,
            absl::implicit_cast<ITfTextLayoutSink*>(this),
            &text_layout_sink_cookie)) &&
        text_layout_sink_cookie != TF_INVALID_COOKIE) {
      wrapper->AddSinkCleaner(
          [source, text_layout_sink_cookie]() mutable {
            source->UnadviseSink(text_layout_sink_cookie);
          });
    }
  }

  std::shared_ptr<PrivateContextWrapper> ExtractPrivateContextIfExists(
      ITfContext* context) {
    const auto it = private_context_map_.find(context);
    if (it == private_context_map_.end()) {
      return nullptr;
    }
    auto retired_node = private_context_map_.extract(it);
    const std::shared_ptr<PrivateContextWrapper> wrapper =
        retired_node.mapped();
    wrapper->get()->GetUiElementManager()->EndAll(this);
    wrapper->Retire();
    return wrapper;
  }

  void RemovePrivateContextIfExists(ITfContext* context) {
    ExtractPrivateContextIfExists(context);
  }

  void UninitPrivateContexts() {
    PrivateContextMap retired_contexts;
    retired_contexts.swap(private_context_map_);
    for (const auto& entry : retired_contexts) {
      entry.second->Retire();
    }
  }

  bool ShouldTreatContextStackAsFocused(
      ITfContext* context, ITfThreadMgr* thread_manager,
      const std::shared_ptr<TipThreadContext>& thread_context) {
    if (thread_manager == nullptr || !IsCurrentActivation(thread_context)) {
      return true;
    }
    if (!thread_has_focus_) {
      return false;
    }
    if (context == nullptr) {
      // An unresolved stack event is treated as active so the security state
      // fails closed.  Only an authoritative identity mismatch may be ignored.
      return true;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> context_document;
    wil::com_ptr_nothrow<ITfDocumentMgr> focused_document;
    if (FAILED(context->GetDocumentMgr(&context_document)) ||
        context_document == nullptr ||
        !IsCurrentActivation(thread_context) ||
        FAILED(thread_manager->GetFocus(&focused_document)) ||
        focused_document == nullptr) {
      return true;
    }
    const auto context_identity =
        ComQuery<IUnknown>(context_document.get());
    const auto focused_identity =
        ComQuery<IUnknown>(focused_document.get());
    if (!context_identity || !focused_identity) {
      return true;
    }
    return IsCurrentActivation(thread_context) &&
           context_identity.get() == focused_identity.get();
  }

  std::shared_ptr<TipPrivateContext> GetFocusedPrivateContext(
      ITfThreadMgr* thread_manager,
      const std::shared_ptr<TipThreadContext>& thread_context) {
    if (thread_manager == nullptr || !IsCurrentActivation(thread_context)) {
      return nullptr;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> focused_document;
    if (FAILED(thread_manager->GetFocus(&focused_document)) ||
        !IsCurrentActivation(thread_context)) {
      return nullptr;
    }
    if (!focused_document) {
      return nullptr;
    }
    wil::com_ptr_nothrow<ITfContext> current_context;
    if (FAILED(focused_document->GetTop(&current_context)) ||
        !IsCurrentActivation(thread_context)) {
      return nullptr;
    }
    return GetPrivateContext(current_context.get());
  }

  HRESULT InitThreadManagerEventSink() {
    // Retrieve the event source for this thread and start advising the
    // ITfThreadMgrEventSink events to this object, i.e. register this object
    // as a listener for the TSF thread events.
    ASSIGN_OR_RETURN_HRESULT(auto source, ComQueryHR<ITfSource>(thread_mgr_));

    const HRESULT result = source->AdviseSink(
        IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
        &thread_mgr_cookie_);

    if (FAILED(result)) {
      thread_mgr_cookie_ = TF_INVALID_COOKIE;
    }

    return result;
  }

  HRESULT UninitThreadManagerEventSink() {
    // If we have started advising the TSF thread events, retrieve the event
    // source for the events and stop advising them.
    if (thread_mgr_cookie_ == TF_INVALID_COOKIE) {
      return S_OK;
    }

    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto source, ComQueryHR<ITfSource>(thread_mgr_));
    const HRESULT result = source->UnadviseSink(thread_mgr_cookie_);
    thread_mgr_cookie_ = TF_INVALID_COOKIE;
    return result;
  }

  HRESULT InitLanguageBar() { return langbar_.InitLangBar(this); }

  HRESULT UninitLanguageBar() { return langbar_.UninitLangBar(); }

  HRESULT InitKeyEventSink() {
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto keystroke,
                             ComQueryHR<ITfKeystrokeMgr>(thread_mgr_));
    return keystroke->AdviseKeyEventSink(
        client_id_, static_cast<ITfKeyEventSink*>(this), TRUE);
  }

  HRESULT UninitKeyEventSink() {
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto keystroke,
                             ComQueryHR<ITfKeystrokeMgr>(thread_mgr_));
    return keystroke->UnadviseKeyEventSink(client_id_);
  }

  HRESULT InitCompartmentEventSink() {
    ASSIGN_OR_RETURN_HRESULT(auto manager,
                             ComQueryHR<ITfCompartmentMgr>(thread_mgr_));

    RETURN_IF_FAILED_HRESULT(AdviseCompartmentEventSink(
        manager.get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
        &keyboard_openclose_cookie_));

    return AdviseCompartmentEventSink(
        manager.get(), GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
        &keyboard_inputmode_conversion_cookie_);
  }

  HRESULT UninitCompartmentEventSink() {
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto manager,
                             ComQueryHR<ITfCompartmentMgr>(thread_mgr_));

    UnadviseCompartmentEventSink(manager.get(),
                                 GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                 &keyboard_openclose_cookie_);
    UnadviseCompartmentEventSink(manager.get(),
                                 GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                                 &keyboard_inputmode_conversion_cookie_);

    return S_OK;
  }

  HRESULT AdviseCompartmentEventSink(ITfCompartmentMgr* manager, REFGUID guid,
                                     DWORD* cookie) {
    if (manager == nullptr || cookie == nullptr) {
      return E_INVALIDARG;
    }

    wil::com_ptr_nothrow<ITfCompartment> compartment;
    RETURN_IF_FAILED_HRESULT(manager->GetCompartment(guid, &compartment));
    ASSIGN_OR_RETURN_HRESULT(auto source, ComQueryHR<ITfSource>(compartment));
    return source->AdviseSink(IID_ITfCompartmentEventSink,
                              static_cast<ITfCompartmentEventSink*>(this),
                              cookie);
  }

  HRESULT UnadviseCompartmentEventSink(ITfCompartmentMgr* manager, REFGUID guid,
                                       DWORD* cookie) {
    if (manager == nullptr || cookie == nullptr) {
      return E_INVALIDARG;
    }

    if (*cookie == TF_INVALID_COOKIE) {
      return E_UNEXPECTED;
    }

    wil::com_ptr_nothrow<ITfCompartment> compartment;
    RETURN_IF_FAILED(manager->GetCompartment(guid, &compartment));
    ASSIGN_OR_RETURN_HRESULT(auto source, ComQueryHR<ITfSource>(compartment));
    const HRESULT result = source->UnadviseSink(*cookie);
    *cookie = TF_INVALID_COOKIE;
    return result;
  }

  HRESULT InitPreservedKey() {
    HRESULT result = S_OK;

    // Retrieve the keyboard-stroke manager from the thread manager, and
    // add the hot keys defined in the kPreservedKeyItems[] array.
    // A keyboard-stroke manager belongs to a thread manager because Windows
    // allows each thread to have its own keyboard (and Language) settings.
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }
    ASSIGN_OR_RETURN_HRESULT(auto keystroke,
                             ComQueryHR<ITfKeystrokeMgr>(thread_mgr_));
    for (const PreserveKeyItem& item : kPreservedKeyItems) {
      // Register a hot key to the keystroke manager.
      result = keystroke->PreserveKey(client_id_, item.guid, &item.key,
                                      item.description, item.length);
      if (SUCCEEDED(result)) {
        preserved_key_map_[item.guid] = item.mapped_vkey;
      }
    }
    return result;
  }

  HRESULT UninitPreservedKey() {
    HRESULT result = S_OK;

    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }
    ASSIGN_OR_RETURN_HRESULT(auto keystroke,
                             ComQueryHR<ITfKeystrokeMgr>(thread_mgr_));

    for (const PreserveKeyItem& item : kPreservedKeyItems) {
      result = keystroke->UnpreserveKey(item.guid, &item.key);
    }
    preserved_key_map_.clear();

    return result;
  }

  HRESULT InitThreadFocusSink() {
    if (thread_focus_cookie_ != TF_INVALID_COOKIE) {
      return S_OK;
    }
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto source, ComQueryHR<ITfSource>(thread_mgr_));
    const HRESULT result = source->AdviseSink(
        IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this),
        &thread_focus_cookie_);
    if (FAILED(result)) {
      thread_focus_cookie_ = TF_INVALID_COOKIE;
    }

    return result;
  }

  HRESULT UninitThreadFocusSink() {
    if (thread_focus_cookie_ == TF_INVALID_COOKIE) {
      return S_OK;
    }
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto source, ComQueryHR<ITfSource>(thread_mgr_));
    const HRESULT result = source->UnadviseSink(thread_focus_cookie_);
    thread_focus_cookie_ = TF_INVALID_COOKIE;

    return result;
  }

  HRESULT InitFunctionProvider() {
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto source,
                             ComQueryHR<ITfSourceSingle>(thread_mgr_));

    return source->AdviseSingleSink(client_id_, IID_ITfFunctionProvider,
                                    static_cast<ITfFunctionProvider*>(this));
  }

  HRESULT UninitFunctionProvider() {
    if (thread_mgr_ == nullptr) {
      return E_FAIL;
    }

    ASSIGN_OR_RETURN_HRESULT(auto source,
                             ComQueryHR<ITfSourceSingle>(thread_mgr_));

    return source->UnadviseSingleSink(client_id_, IID_ITfFunctionProvider);
  }

  HRESULT InitDisplayAttributes() {
    wil::com_ptr_nothrow<ITfCategoryMgr> category = category_;
    if (!category) {
      return E_UNEXPECTED;
    }

    // register the display attribute for input strings and the one for
    // converted strings.
    RETURN_IF_FAILED_HRESULT(category->RegisterGUID(
        TipDisplayAttributeInput::guid(), &input_attribute_));
    return category->RegisterGUID(TipDisplayAttributeConverted::guid(),
                                  &converted_attribute_);
  }

  HRESULT InitTaskWindow() {
    if (::IsWindow(task_window_handle_)) {
      return S_FALSE;
    }
    task_window_handle_ =
        ::CreateWindowExW(0, kTaskWindowClassName, L"", 0, 0, 0, 0, 0,
                          HWND_MESSAGE, nullptr, g_module, nullptr);
    if (!::IsWindow(task_window_handle_)) {
      return E_FAIL;
    }
    return S_OK;
  }

  HRESULT UninitTaskWindow() {
    if (!::IsWindow(task_window_handle_)) {
      return S_FALSE;
    }

    ::KillTimer(task_window_handle_, kDelayedSessionCommandTimerId);
    has_pending_delayed_session_command_ = false;
    pending_delayed_session_command_.Clear();
    pending_delayed_session_context_.reset();
    pending_delayed_session_focus_epoch_ = 0;
    pending_delayed_session_focus_revision_ = 0;
    pending_delayed_session_composition_generation_ = 0;

    ::DestroyWindow(task_window_handle_);
    task_window_handle_ = nullptr;
    return S_OK;
  }

  static LRESULT WINAPI TaskWindowProc(HWND window_handle,
                                      UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam) {
    TipTextServiceImpl* self = Self();
    if (self == nullptr) {
      return ::DefWindowProcW(window_handle, message, wparam, lparam);
    }

    if (window_handle == self->task_window_handle_) {
      if (message == kUpdateUIMessage) {
        self->OnUpdateUI();
        return 0;
      }

      if (message == WM_TIMER &&
          wparam == kDelayedSessionCommandTimerId) {
        self->OnDelayedSessionCommandTimer();
        return 0;
      }
    }

    return ::DefWindowProcW(window_handle, message, wparam, lparam);
  }

  void OnDelayedSessionCommandTimer() {
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    const HWND task_window = task_window_handle_;
    if (!thread_context || !::IsWindow(task_window) ||
        !IsCurrentActivation(thread_context)) {
      return;
    }

    ::KillTimer(task_window, kDelayedSessionCommandTimerId);
    if (!IsCurrentActivation(thread_context)) {
      return;
    }

    if (!has_pending_delayed_session_command_) {
      return;
    }

    commands::SessionCommand command = pending_delayed_session_command_;
    wil::com_ptr_nothrow<ITfContext> context =
        pending_delayed_session_context_;
    const uint64_t scheduled_focus_epoch =
        pending_delayed_session_focus_epoch_;
    const int32_t scheduled_focus_revision =
        pending_delayed_session_focus_revision_;
    uint64_t scheduled_composition_generation =
        pending_delayed_session_composition_generation_;

    has_pending_delayed_session_command_ = false;
    pending_delayed_session_command_.Clear();
    pending_delayed_session_context_.reset();
    pending_delayed_session_focus_epoch_ = 0;
    pending_delayed_session_focus_revision_ = 0;
    pending_delayed_session_composition_generation_ = 0;

    const std::shared_ptr<TipPrivateContext> private_context =
        context ? GetPrivateContext(context.get()) : nullptr;
    if (!context || !private_context || scheduled_focus_epoch == 0 ||
        scheduled_focus_epoch != thread_context->GetGrimodexFocusEpoch() ||
        scheduled_focus_revision != thread_context->GetFocusRevision() ||
        !IsCurrentActivation(thread_context)) {
      return;
    }

    // The first output of a composition can schedule a delayed callback before
    // UpdateContext starts the TSF composition.  Resolve that one zero token at
    // timer time; otherwise require the exact composition that scheduled the
    // callback so a commit/cancel/restart cannot receive stale output.
    if (scheduled_composition_generation == 0) {
      scheduled_composition_generation =
          private_context->composition_generation();
    }
    if (!private_context->IsCompositionForFocusDomainAndGeneration(
            scheduled_focus_epoch, scheduled_focus_revision,
            scheduled_composition_generation)) {
      return;
    }

    TipEditSession::SendDelayedSessionCommandAsync(
        this, context.get(), command, scheduled_focus_epoch,
        scheduled_focus_revision,
        scheduled_composition_generation);
  }

  void OnUpdateUI() {
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return;
    }
    wil::com_ptr_nothrow<ITfDocumentMgr> document_manager;
    if (FAILED(thread_manager->GetFocus(&document_manager)) ||
        !IsCurrentActivation(thread_context)) {
      return;
    }
    if (!document_manager) {
      return;
    }
    wil::com_ptr_nothrow<ITfContext> context;
    if (FAILED(document_manager->GetTop(&context)) ||
        !IsCurrentActivation(thread_context)) {
      return;
    }
    if (!context) {
      return;
    }
    UpdateUiEditSessionImpl::BeginRequest(this, context.get());
  }

  HRESULT InitRendererCallbackWindow() {
    if (::IsWindow(renderer_callback_window_handle_)) {
      return S_FALSE;
    }
    renderer_callback_window_handle_ =
        ::CreateWindowExW(0, kMessageReceiverClassName, L"", 0, 0, 0, 0, 0,
                          HWND_MESSAGE, nullptr, g_module, nullptr);
    if (!::IsWindow(renderer_callback_window_handle_)) {
      return E_FAIL;
    }

    // Do not care about thread safety.
    static UINT renderer_callback_message =
        ::RegisterWindowMessage(mozc::kMessageReceiverMessageName);
    static UINT renderer_highlight_callback_message =
        ::RegisterWindowMessage(mozc::kMessageReceiverHighlightMessageName);

    if (!WindowUtil::ChangeMessageFilter(renderer_callback_window_handle_,
                                         renderer_callback_message) ||
        !WindowUtil::ChangeMessageFilter(
            renderer_callback_window_handle_,
            renderer_highlight_callback_message)) {
      ::DestroyWindow(renderer_callback_window_handle_);
      renderer_callback_window_handle_ = nullptr;
      return E_FAIL;
    }
    return S_OK;
  }

  HRESULT UninitRendererCallbackWindow() {
    SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                  /*focus_revision=*/0,
                                  /*output_generation=*/0);
    if (!::IsWindow(renderer_callback_window_handle_)) {
      return S_FALSE;
    }
    ::DestroyWindow(renderer_callback_window_handle_);
    renderer_callback_window_handle_ = nullptr;
    return S_OK;
  }

  static LRESULT WINAPI RendererCallbackWidnowProc(HWND window_handle,
                                                   UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
    TipTextServiceImpl* self = Self();
    if (self == nullptr) {
      return ::DefWindowProcW(window_handle, message, wparam, lparam);
    }

    // Do not care about thread safety.
    static UINT renderer_callback_message =
        ::RegisterWindowMessage(mozc::kMessageReceiverMessageName);
    static UINT renderer_highlight_callback_message =
        ::RegisterWindowMessage(mozc::kMessageReceiverHighlightMessageName);
    if (window_handle == self->renderer_callback_window_handle_) {
      switch (commands::GetRendererCallbackKindForMessage(
          message, renderer_callback_message,
          renderer_highlight_callback_message)) {
        case commands::RendererCallbackKind::kSelect:
          self->OnRendererCallback(commands::SessionCommand::SELECT_CANDIDATE,
                                   wparam, lparam);
          return 0;
        case commands::RendererCallbackKind::kHighlight:
          self->OnRendererCallback(
              commands::SessionCommand::HIGHLIGHT_CANDIDATE, wparam, lparam);
          return 0;
        case commands::RendererCallbackKind::kUnsupported:
          break;
      }
    }
    return ::DefWindowProcW(window_handle, message, wparam, lparam);
  }

  void OnRendererCallback(commands::SessionCommand::CommandType type,
                          WPARAM wparam, LPARAM lparam) {
    const uint64_t callback_token = static_cast<uint64_t>(wparam);
    const std::shared_ptr<TipThreadContext> thread_context =
        GetThreadContextLease();
    wil::com_ptr_nothrow<ITfThreadMgr> thread_manager = thread_mgr_;
    if (!thread_context || !thread_manager) {
      return;
    }
    const commands::RendererCallbackProvenance expected_provenance = {
        .token = renderer_callback_token_,
        .focus_epoch = renderer_callback_focus_epoch_,
        .focus_revision = renderer_callback_focus_revision_,
        .output_generation = renderer_callback_output_generation_,
    };
    const auto is_current = [this, callback_token, &thread_context]() {
      const commands::RendererCallbackProvenance current_provenance = {
          .token = renderer_callback_token_,
          .focus_epoch = renderer_callback_focus_epoch_,
          .focus_revision = renderer_callback_focus_revision_,
          .output_generation = renderer_callback_output_generation_,
      };
      return IsCurrentActivation(thread_context) &&
             commands::IsRendererCallbackProvenanceCurrent(
                 current_provenance, callback_token,
                 thread_context->GetGrimodexFocusEpoch(),
                 thread_context->GetFocusRevision(),
                 thread_context->GetInputModeManager()
                     ->IsPasswordInputScope(),
                 renderer_callback_output_generation_);
    };
    if (!is_current()) {
      return;
    }
    const TsfFocusSnapshot renderer_domain = {
        .focus_epoch = expected_provenance.focus_epoch,
        .focus_revision = expected_provenance.focus_revision,
    };
    wil::com_ptr_nothrow<ITfDocumentMgr> document_manager;
    if (FAILED(thread_manager->GetFocus(&document_manager))) {
      return;
    }
    if (!document_manager) {
      return;
    }
    wil::com_ptr_nothrow<ITfContext> context;
    if (FAILED(document_manager->GetTop(&context))) {
      return;
    }
    if (!context) {
      return;
    }
    if (!is_current()) {
      return;
    }
    const std::shared_ptr<TipPrivateContext> private_context =
        GetPrivateContext(context.get());
    if (!private_context ||
        !private_context->IsLastOutputForFocusDomainAndGeneration(
            renderer_domain.focus_epoch, renderer_domain.focus_revision,
            expected_provenance.output_generation)) {
      return;
    }
    if (type == commands::SessionCommand::SELECT_CANDIDATE) {
      SetRendererCallbackProvenance(/*token=*/0, /*focus_epoch=*/0,
                                    /*focus_revision=*/0,
                                    /*output_generation=*/0);
    }
    TipEditSession::OnRendererCallbackAsync(
        this, context.get(), type, static_cast<int32_t>(lparam),
        renderer_domain.focus_epoch, renderer_domain.focus_revision,
        expected_provenance.output_generation);
  }

  // Represents the status of the thread manager which owns this IME object.
  wil::com_ptr_nothrow<ITfThreadMgr> thread_mgr_;

  // Represents the ID of the client application using this IME object.
  TfClientId client_id_;

  // Stores the flag passed to ActivateEx.
  DWORD activate_flags_;

  // Represents the cookie ID for the thread manager.
  DWORD thread_mgr_cookie_;

  // The cookie issued for installing ITfThreadFocusSink.
  DWORD thread_focus_cookie_;

  // The cookie issued for installing ITfCompartmentEventSink.
  DWORD keyboard_openclose_cookie_;
  DWORD keyboard_inputmode_conversion_cookie_;

  // The category manager object to register or query a GUID.
  wil::com_ptr_nothrow<ITfCategoryMgr> category_;

  // Represents the display attributes.
  TfGuidAtom input_attribute_;
  TfGuidAtom converted_attribute_;

  // Used for LangBar integration.
  TipLangBar langbar_;

  using PreservedKeyMap = absl::flat_hash_map<GUID, UINT, GuidHash>;
  using PrivateContextMap =
      absl::flat_hash_map<wil::com_ptr_nothrow<ITfContext>,
                          std::shared_ptr<PrivateContextWrapper>,
                          ComPtrHash<ITfContext>>;
  PrivateContextMap private_context_map_;
  bool private_contexts_accepting_ = false;
  std::shared_ptr<TipPrivateContext> active_private_context_;
  PreservedKeyMap preserved_key_map_;
  std::shared_ptr<TipThreadContext> thread_context_;
  uint64_t next_activation_focus_epoch_ = 1;
  bool activation_active_ = false;
  bool thread_has_focus_ = false;
  uint64_t thread_focus_callback_generation_ = 0;
  uint64_t conversion_mode_callback_generation_ = 0;
  uint64_t open_close_mode_callback_generation_ = 0;
  bool activating_ = false;
  bool activation_cancel_requested_ = false;
  bool deactivating_ = false;
  int callback_suspension_depth_ = 0;
  bool pending_context_resync_ = false;
  std::vector<std::pair<bool, wil::com_ptr_nothrow<ITfContext>>>
      pending_context_lifecycle_events_;
  HWND task_window_handle_;
  HWND renderer_callback_window_handle_;
  uint64_t renderer_callback_token_ = 0;
  uint64_t renderer_callback_focus_epoch_ = 0;
  int32_t renderer_callback_focus_revision_ = 0;
  uint64_t renderer_callback_output_generation_ = 0;

  bool has_pending_delayed_session_command_;
  commands::SessionCommand pending_delayed_session_command_;
  wil::com_ptr_nothrow<ITfContext> pending_delayed_session_context_;
  uint64_t pending_delayed_session_focus_epoch_ = 0;
  int32_t pending_delayed_session_focus_revision_ = 0;
  uint64_t pending_delayed_session_composition_generation_ = 0;
};

}  // namespace

wil::com_ptr_nothrow<TipTextService> TipTextServiceFactory::Create() {
  return MakeComPtr<TipTextServiceImpl>();
}

bool TipTextServiceFactory::OnDllProcessAttach(HINSTANCE module_handle,
                                               bool static_loading) {
  g_module = module_handle;
  g_tls_index = ::TlsAlloc();
  if (!TipTextServiceImpl::OnDllProcessAttach(module_handle)) {
    return false;
  }
  return true;
}

void TipTextServiceFactory::OnDllProcessDetach(HINSTANCE module_handle,
                                               bool process_shutdown) {
  TipTextServiceImpl::OnDllProcessDetach(module_handle);

  if (g_tls_index != TLS_OUT_OF_INDEXES) {
    ::TlsFree(g_tls_index);
    g_tls_index = TLS_OUT_OF_INDEXES;
  }
  g_module_unloaded = true;
  g_module = nullptr;
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
