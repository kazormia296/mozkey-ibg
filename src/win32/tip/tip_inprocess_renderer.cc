// Copyright 2010-2021, Google Inc.
// All rights reserved.

#include "win32/tip/tip_inprocess_renderer.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

#include "base/const.h"
#include "client/client_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/renderer_callback_provenance.h"
#include "protocol/renderer_command.pb.h"
#include "renderer/renderer_style_config.h"
#include "renderer/win32/window_manager.h"

namespace mozc {
namespace win32 {
namespace tsf {
namespace {

constexpr UINT kInProcessRendererUpdateMessage = WM_APP + 0x531;
constexpr UINT kInProcessRendererShutdownMessage = WM_APP + 0x532;

class InProcessRendererSendCommand final
    : public client::SendCommandInterface {
 public:
  bool SendCommand(const commands::SessionCommand& command,
                   commands::Output* output) override {
    // Mozkey IbG: same transport as RendererServerSendCommand in
    // renderer/renderer_server.cc. WPARAM carries the renderer callback
    // provenance token (not the command type, as in upstream), so each
    // supported command type has its own registered message.
    const wchar_t* message_name = nullptr;
    switch (commands::GetRendererCallbackKind(command.type())) {
      case commands::RendererCallbackKind::kSelect:
        message_name = kMessageReceiverMessageName;
        break;
      case commands::RendererCallbackKind::kHighlight:
        message_name = kMessageReceiverHighlightMessageName;
        break;
      case commands::RendererCallbackKind::kSubmit:
        message_name = kMessageReceiverSubmitMessageName;
        break;
      case commands::RendererCallbackKind::kUnsupported:
        return false;
    }

    HWND target =
        reinterpret_cast<HWND>(static_cast<uintptr_t>(receiver_handle_));
    if (target == nullptr || !::IsWindow(target)) {
      return false;
    }

    if (!command.has_renderer_callback_token() ||
        !commands::IsRendererCallbackTokenTransportable(
            command.renderer_callback_token(), renderer_callback_token_,
            std::numeric_limits<UINT_PTR>::max())) {
      return false;
    }

    const UINT mozc_message = ::RegisterWindowMessageW(message_name);
    if (mozc_message == 0) {
      return false;
    }

    return ::PostMessageW(
               target, mozc_message,
               static_cast<WPARAM>(
                   static_cast<UINT_PTR>(command.renderer_callback_token())),
               static_cast<LPARAM>(command.id())) != FALSE;
  }

  void set_receiver_handle(uint32_t receiver_handle) {
    receiver_handle_ = receiver_handle;
  }
  void set_renderer_callback_token(uint64_t token) {
    renderer_callback_token_ = token;
  }

 private:
  uint32_t receiver_handle_ = 0;
  uint64_t renderer_callback_token_ = 0;
};

struct RendererUpdatePayload {
  commands::RendererCommand command;
  HWND host_window = nullptr;
};

bool IsCurrentProcessWindow(HWND hwnd) {
  if (hwnd == nullptr || !::IsWindow(hwnd)) {
    return false;
  }
  DWORD process_id = 0;
  if (::GetWindowThreadProcessId(hwnd, &process_id) == 0) {
    return false;
  }
  return process_id == ::GetCurrentProcessId();
}

class InProcessRendererWorker final {
 public:
  InProcessRendererWorker() = default;
  InProcessRendererWorker(const InProcessRendererWorker&) = delete;
  InProcessRendererWorker& operator=(const InProcessRendererWorker&) = delete;

  ~InProcessRendererWorker() { Shutdown(); }

  bool PostUpdate(const commands::RendererCommand& command, HWND host_window) {
    if (!EnsureStarted()) {
      return false;
    }

    auto payload = std::make_unique<RendererUpdatePayload>();
    payload->command = command;
    payload->host_window = host_window;

    DWORD thread_id = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      thread_id = thread_id_;
    }
    if (thread_id == 0 ||
        !::PostThreadMessageW(thread_id, kInProcessRendererUpdateMessage, 0,
                              reinterpret_cast<LPARAM>(payload.get()))) {
      return false;
    }
    payload.release();
    return true;
  }

  void Shutdown() {
    DWORD thread_id = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!thread_.joinable()) {
        return;
      }
      thread_id = thread_id_;
    }

    if (thread_id != 0) {
      ::PostThreadMessageW(thread_id, kInProcessRendererShutdownMessage, 0, 0);
    }
    thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    thread_id_ = 0;
    queue_ready_ = false;
  }

 private:
  bool EnsureStarted() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (thread_.joinable()) {
      return queue_ready_ && thread_id_ != 0;
    }

    queue_ready_ = false;
    thread_id_ = 0;
    thread_ = std::thread([this]() { ThreadMain(); });
    ready_cv_.wait(lock, [this]() { return queue_ready_; });
    return thread_id_ != 0;
  }

  void ThreadMain() {
    // Force creation of this thread's message queue before publishing the
    // thread id. CandidateWindow instances created below then receive and
    // dispatch their mouse/window messages on this dedicated renderer thread,
    // rather than on SearchHost's TSF UI thread.
    MSG initial_message = {};
    ::PeekMessageW(&initial_message, nullptr, 0, 0, PM_NOREMOVE);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      thread_id_ = ::GetCurrentThreadId();
      queue_ready_ = true;
    }
    ready_cv_.notify_all();

    InProcessRendererSendCommand send_command;
    std::unique_ptr<renderer::win32::WindowManager> window_manager;
    HWND current_host_window = nullptr;

    MSG message = {};
    while (true) {
      const BOOL get_message_result = ::GetMessageW(&message, nullptr, 0, 0);
      if (get_message_result <= 0) {
        break;
      }

      if (message.hwnd == nullptr &&
          message.message == kInProcessRendererShutdownMessage) {
        break;
      }

      if (message.hwnd == nullptr &&
          message.message == kInProcessRendererUpdateMessage) {
        std::unique_ptr<RendererUpdatePayload> payload(
            reinterpret_cast<RendererUpdatePayload*>(message.lParam));
        if (payload == nullptr) {
          continue;
        }

        renderer::UpdateRendererStyleFromConfig();

        // Callbacks are accepted only for the token of the latest visible
        // update, mirroring RendererServer.
        send_command.set_renderer_callback_token(
            payload->command.visible() &&
                    payload->command.has_application_info() &&
                    payload->command.application_info()
                        .has_renderer_callback_token()
                ? payload->command.application_info().renderer_callback_token()
                : 0);

        if (payload->command.has_application_info() &&
            payload->command.application_info().has_receiver_handle()) {
          send_command.set_receiver_handle(
              payload->command.application_info().receiver_handle());
        }

        if (!payload->command.visible() ||
            !IsCurrentProcessWindow(payload->host_window)) {
          if (window_manager != nullptr) {
            window_manager->HideAllWindows();
          }
          continue;
        }

        if (window_manager == nullptr ||
            current_host_window != payload->host_window) {
          if (window_manager != nullptr) {
            window_manager->DestroyAllWindows();
            window_manager.reset();
          }

          auto manager = std::make_unique<renderer::win32::WindowManager>();
          manager->SetSendCommandInterface(&send_command);
          // The in-process renderer owns a dedicated message loop and shuts it
          // down explicitly. Destroying an individual renderer window must not
          // post WM_QUIT.
          manager->Initialize(payload->host_window, false);
          if (!manager->IsAvailable()) {
            manager->DestroyAllWindows();
            continue;
          }
          current_host_window = payload->host_window;
          window_manager = std::move(manager);
        }

        window_manager->UpdateLayout(payload->command);
        continue;
      }

      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }

    if (window_manager != nullptr) {
      window_manager->DestroyAllWindows();
      window_manager.reset();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::thread thread_;
  DWORD thread_id_ = 0;
  bool queue_ready_ = false;
};

thread_local std::unique_ptr<InProcessRendererWorker> g_inprocess_worker;

}  // namespace

void TipInProcessRenderer::OnUpdated(
    const commands::RendererCommand& command, HWND host_window) {
  if (command.type() != commands::RendererCommand::UPDATE) {
    return;
  }

  if (!command.visible()) {
    if (g_inprocess_worker != nullptr) {
      g_inprocess_worker->PostUpdate(command, host_window);
    }
    return;
  }

  if (!IsCurrentProcessWindow(host_window)) {
    if (g_inprocess_worker != nullptr) {
      g_inprocess_worker->PostUpdate(command, host_window);
    }
    return;
  }

  if (g_inprocess_worker == nullptr) {
    g_inprocess_worker = std::make_unique<InProcessRendererWorker>();
  }
  g_inprocess_worker->PostUpdate(command, host_window);
}

void TipInProcessRenderer::OnUIThreadUninitialized() {
  if (g_inprocess_worker != nullptr) {
    g_inprocess_worker->Shutdown();
    g_inprocess_worker.reset();
  }
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
