#include "session/zenz_named_pipe_client.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "zenz/zenz_wire_protocol.h"

#if defined(_WIN32)
#include <windows.h>
#pragma comment(lib, "Advapi32.lib")
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif  // _WIN32

namespace mozc {
namespace session {
namespace {

using ::mozc::zenz::kZenzWireKindRequest;
using ::mozc::zenz::kZenzWireKindResponse;
using ::mozc::zenz::kZenzWireMagic;
using ::mozc::zenz::kZenzWireStatusOk;
using ::mozc::zenz::kZenzWireStatusTimeout;
using ::mozc::zenz::ZenzWireResponseHeader;

// Mozkey IbG extends upstream's v1 scorer request with the inference device
// selected by Config.zenz_live_correction_device.  The request header carries
// one extra length field and the device name follows the prompt, so this
// transport and the Mozkey IbG scorer (zenz_scorer/main.cc) speak version 2.
// Keep this layout in sync with zenz_scorer/main.cc.  Magic, kinds, statuses
// and the response header are shared with //zenz:wire_protocol.
constexpr uint16_t kZenzWireVersion = 2;
constexpr uint32_t kMaxBackendDeviceBytes = 128;

#pragma pack(push, 1)
struct ZenzWireRequestHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t kind;
  uint32_t generation;
  uint32_t timeout_msec;
  uint32_t max_output_chars;
  uint32_t prompt_size;
  uint32_t backend_device_size;
};
#pragma pack(pop)

static_assert(sizeof(ZenzWireRequestHeader) == 28);

constexpr uint32_t kMaxPromptBytes = 8192;
// Response payload caps.  Upstream bounds the value and the debug string
// separately; Mozkey IbG additionally bounds their sum before allocating.
constexpr uint32_t kMaxResponseValueBytes = 64 * 1024;
constexpr uint32_t kMaxResponseDebugBytes = 64 * 1024;
constexpr uint32_t kMaxWirePayloadBytes = 64 * 1024;

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

enum class PipeIoResult {
  kOk,
  kTimeout,
  kError,
};

#if defined(_WIN32)

void ZenzPipeDebugOutput(const std::wstring& message);

class ScopedWinHandle {
 public:
  explicit ScopedWinHandle(HANDLE handle = nullptr) : handle_(handle) {}
  ~ScopedWinHandle() { Reset(); }

  ScopedWinHandle(const ScopedWinHandle&) = delete;
  ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

  HANDLE get() const { return handle_; }

  void Reset(HANDLE handle = nullptr) {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_;
};

DWORD RemainingMsec(Deadline deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now());
  if (remaining.count() <= 0) {
    return 0;
  }
  return static_cast<DWORD>(std::min<int64_t>(
      std::max<int64_t>(1, remaining.count()),
      std::numeric_limits<DWORD>::max()));
}

bool SleepWithinDeadline(DWORD requested_msec, Deadline deadline,
                         HANDLE cancellation_event) {
  const DWORD remaining = RemainingMsec(deadline);
  if (remaining == 0) {
    return false;
  }
  return ::WaitForSingleObject(cancellation_event,
                               std::min(requested_msec, remaining)) ==
             WAIT_TIMEOUT &&
         RemainingMsec(deadline) > 0;
}

void CancelAndReap(HANDLE handle, OVERLAPPED* overlapped) {
  if (!::CancelIoEx(handle, overlapped)) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_NOT_FOUND) {
      ZenzPipeDebugOutput(
          std::wstring(L"CancelIoEx failed error=")
              .append(std::to_wstring(error)));
    }
  }

  DWORD ignored = 0;
  if (!::GetOverlappedResult(handle, overlapped, &ignored, TRUE)) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_OPERATION_ABORTED) {
      ZenzPipeDebugOutput(
          std::wstring(L"GetOverlappedResult after cancel failed error=")
              .append(std::to_wstring(error)));
    }
  }
}

PipeIoResult WaitForPendingIo(HANDLE handle, OVERLAPPED* overlapped,
                              Deadline deadline, DWORD* transferred,
                              HANDLE cancellation_event) {
  const DWORD remaining = RemainingMsec(deadline);
  if (remaining == 0) {
    CancelAndReap(handle, overlapped);
    return PipeIoResult::kTimeout;
  }
  const HANDLE events[] = {cancellation_event, overlapped->hEvent};
  const DWORD wait = ::WaitForMultipleObjects(2, events, FALSE, remaining);
  if (wait == WAIT_OBJECT_0 + 1) {
    return ::GetOverlappedResult(handle, overlapped, transferred, FALSE)
               ? PipeIoResult::kOk
               : PipeIoResult::kError;
  }
  CancelAndReap(handle, overlapped);
  return wait == WAIT_TIMEOUT ? PipeIoResult::kTimeout : PipeIoResult::kError;
}

PipeIoResult WriteSomeUntil(HANDLE handle, const void* data, DWORD size,
                            Deadline deadline, DWORD* transferred,
                            HANDLE cancellation_event) {
  if (::WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
    return PipeIoResult::kError;
  }
  ScopedWinHandle event(
      ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (event.get() == nullptr) {
    return PipeIoResult::kError;
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = event.get();

  if (::WriteFile(handle, data, size, nullptr, &overlapped)) {
    return ::GetOverlappedResult(
               handle, &overlapped, transferred, FALSE)
               ? PipeIoResult::kOk
               : PipeIoResult::kError;
  }

  if (::GetLastError() != ERROR_IO_PENDING) {
    return PipeIoResult::kError;
  }
  return WaitForPendingIo(handle, &overlapped, deadline, transferred,
                          cancellation_event);
}

PipeIoResult ReadSomeUntil(HANDLE handle, void* data, DWORD size,
                           Deadline deadline, DWORD* transferred,
                           HANDLE cancellation_event) {
  if (::WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
    return PipeIoResult::kError;
  }
  ScopedWinHandle event(
      ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (event.get() == nullptr) {
    return PipeIoResult::kError;
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = event.get();

  if (::ReadFile(handle, data, size, nullptr, &overlapped)) {
    return ::GetOverlappedResult(
               handle, &overlapped, transferred, FALSE)
               ? PipeIoResult::kOk
               : PipeIoResult::kError;
  }

  if (::GetLastError() != ERROR_IO_PENDING) {
    return PipeIoResult::kError;
  }
  return WaitForPendingIo(handle, &overlapped, deadline, transferred,
                          cancellation_event);
}

PipeIoResult WriteAllUntil(HANDLE handle, const void* data, uint32_t size,
                           Deadline deadline, HANDLE cancellation_event) {
  const uint8_t* ptr = static_cast<const uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    DWORD written = 0;
    const PipeIoResult result = WriteSomeUntil(handle, ptr, remaining, deadline,
                                               &written, cancellation_event);
    if (result != PipeIoResult::kOk) {
      return result;
    }
    if (written == 0) {
      return PipeIoResult::kError;
    }
    ptr += written;
    remaining -= written;
  }
  return PipeIoResult::kOk;
}

PipeIoResult ReadAllUntil(HANDLE handle, void* data, uint32_t size,
                          Deadline deadline, HANDLE cancellation_event) {
  uint8_t* ptr = static_cast<uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    DWORD read = 0;
    const PipeIoResult result = ReadSomeUntil(handle, ptr, remaining, deadline,
                                              &read, cancellation_event);
    if (result != PipeIoResult::kOk) {
      return result;
    }
    if (read == 0) {
      return PipeIoResult::kError;
    }
    ptr += read;
    remaining -= read;
  }
  return PipeIoResult::kOk;
}

std::wstring Utf8ToWidePipeName(const std::string& s) {
  if (s.empty()) {
    return L"";
  }

  const int size = ::MultiByteToWideChar(
      CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (size <= 0) {
    return L"";
  }

  std::wstring output(size, L'\0');
  ::MultiByteToWideChar(
      CP_UTF8, 0, s.data(), static_cast<int>(s.size()), output.data(), size);
  return output;
}

void ZenzPipeDebugOutput(const std::wstring& message) {
  std::wstring line = L"[zenz-pipe] ";
  line.append(message);
  line.push_back(L'\n');
  ::OutputDebugStringW(line.c_str());
}

std::wstring RedactedStatsWide(const wchar_t* label, size_t bytes) {
  std::wstring output(label);
  output.append(L"_bytes=");
  output.append(std::to_wstring(bytes));
  return output;
}

HANDLE TryOpenPipeOnce(const std::wstring& pipe_name) {
  return ::CreateFileW(
      pipe_name.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
      nullptr);
}

std::wstring GetCurrentModuleDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD size = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (size == 0 || size >= MAX_PATH) {
    return L".";
  }

  std::wstring full(path, size);
  const size_t pos = full.find_last_of(L"\\/");
  if (pos == std::wstring::npos) {
    return L".";
  }

  return full.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& file) {
  if (dir.empty()) {
    return file;
  }
  if (dir.back() == L'\\' || dir.back() == L'/') {
    return dir + file;
  }
  return dir + L"\\" + file;
}

// The pipe name is predictable, so bind the connected handle to the server's
// kernel-reported process image, session, and user SID before sending input.
// Do not canonicalize a path from an untrusted process by reopening it: a
// same-user attacker could swap the path/symlink between launch and checking.
bool ReadTokenUser(HANDLE process, std::vector<BYTE>* data) {
  HANDLE raw_token = nullptr;
  if (!::OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
    return false;
  }
  ScopedWinHandle token(raw_token);
  DWORD bytes = 0;
  if (::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes) ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      bytes < sizeof(TOKEN_USER) || bytes > 65536) {
    return false;
  }
  data->resize(bytes);
  return ::GetTokenInformation(token.get(), TokenUser, data->data(), bytes,
                               &bytes) != FALSE;
}

bool IsTrustedScorerPipeServer(HANDLE pipe) {
  ULONG server_pid = 0;
  if (!::GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid == 0 ||
      server_pid == ::GetCurrentProcessId()) {
    return false;
  }

  ScopedWinHandle server_process(
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_pid));
  if (server_process.get() == nullptr) {
    return false;
  }

  DWORD client_session = 0;
  DWORD server_session = 0;
  if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &client_session) ||
      !::ProcessIdToSessionId(server_pid, &server_session) ||
      client_session != server_session) {
    return false;
  }

  std::vector<BYTE> client_token_user;
  std::vector<BYTE> server_token_user;
  if (!ReadTokenUser(::GetCurrentProcess(), &client_token_user) ||
      !ReadTokenUser(server_process.get(), &server_token_user)) {
    return false;
  }
  const auto* client_user =
      reinterpret_cast<const TOKEN_USER*>(client_token_user.data());
  const auto* server_user =
      reinterpret_cast<const TOKEN_USER*>(server_token_user.data());
  if (!::EqualSid(client_user->User.Sid, server_user->User.Sid)) {
    return false;
  }

  std::wstring image_path(32768, L'\0');
  DWORD image_chars = static_cast<DWORD>(image_path.size());
  if (!::QueryFullProcessImageNameW(server_process.get(), 0,
                                   image_path.data(), &image_chars) ||
      image_chars == 0) {
    return false;
  }
  image_path.resize(image_chars);

  const std::wstring module_dir = GetCurrentModuleDirectory();
  if (module_dir.empty() || module_dir == L".") {
    return false;
  }
  const std::wstring expected_path =
      JoinPath(module_dir, L"mozc_zenz_scorer.exe");
  // Compare the process's image name captured by Windows, not the contents of
  // a path that can be redirected after the process started. A protected
  // installation directory is a necessary part of this security boundary.
  return ::CompareStringOrdinal(expected_path.c_str(), -1,
                                image_path.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool LaunchZenzScorerIfNeeded() {
  // Do not make scorer launch a one-shot decision.  The scorer process may be
  // killed independently from mozc_server.exe during development, upgrade, or
  // crash recovery.  Throttle only very recent launch attempts to avoid spawning
  // many scorer processes while the pipe is still being created.
  static std::atomic<DWORD> last_launch_tick{0};

  constexpr DWORD kLaunchThrottleMsec = 2000;

  const DWORD now = ::GetTickCount();
  DWORD previous = last_launch_tick.load();

  if (previous != 0 && now - previous < kLaunchThrottleMsec) {
    ZenzPipeDebugOutput(
        std::wstring(L"scorer launch throttled elapsed_msec=")
            .append(std::to_wstring(now - previous)));
    return false;
  }

  while (!last_launch_tick.compare_exchange_weak(previous, now)) {
    if (previous != 0 && now - previous < kLaunchThrottleMsec) {
      ZenzPipeDebugOutput(
          std::wstring(L"scorer launch throttled elapsed_msec=")
              .append(std::to_wstring(now - previous)));
      return false;
    }
  }

  const std::wstring dir = GetCurrentModuleDirectory();
  const std::wstring scorer_path = JoinPath(dir, L"mozc_zenz_scorer.exe");

  const DWORD attr = ::GetFileAttributesW(scorer_path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES ||
      (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    ZenzPipeDebugOutput(L"scorer not found");
    return false;
  }

  std::wstring command_line = L"\"";
  command_line.append(scorer_path);
  command_line.append(L"\"");

  std::vector<wchar_t> command_line_buffer(
      command_line.begin(), command_line.end());
  command_line_buffer.push_back(L'\0');

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION process = {};

  ZenzPipeDebugOutput(L"launch scorer");

  if (!::CreateProcessW(
          nullptr,
          command_line_buffer.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_NO_WINDOW,
          nullptr,
          dir.c_str(),
          &startup,
          &process)) {
    const DWORD error = ::GetLastError();
    ZenzPipeDebugOutput(
        std::wstring(L"launch scorer failed error=")
            .append(std::to_wstring(error)));
    return false;
  }

  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return true;
}

HANDLE OpenPipeWithAutoLaunch(const std::wstring& pipe_name,
                              uint32_t request_timeout_msec,
                              Deadline transport_deadline,
                              HANDLE cancellation_event) {
  if (::WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
    return INVALID_HANDLE_VALUE;
  }
  ZenzPipeDebugOutput(
      std::wstring(L"CreateFileW begin ")
          .append(RedactedStatsWide(
              L"pipe_name", pipe_name.size() * sizeof(wchar_t))));

  HANDLE pipe = TryOpenPipeOnce(pipe_name);
  if (pipe != INVALID_HANDLE_VALUE) {
    ZenzPipeDebugOutput(L"CreateFileW succeeded");
    return pipe;
  }

  const DWORD error = ::GetLastError();
  ZenzPipeDebugOutput(
      std::wstring(L"CreateFileW failed error=")
          .append(std::to_wstring(error)));

  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    const bool launched = LaunchZenzScorerIfNeeded();
    ZenzPipeDebugOutput(
        std::wstring(L"scorer launch requested launched=")
            .append(launched ? L"true" : L"false"));

    constexpr DWORD kColdStartRetryMsec = 50;
    const uint32_t retry_budget_msec =
        std::max<uint32_t>(request_timeout_msec, kColdStartRetryMsec);
    const Deadline retry_deadline = std::min(
        transport_deadline,
        Clock::now() + std::chrono::milliseconds(retry_budget_msec));

    int attempt = 0;
    while (::WaitForSingleObject(cancellation_event, 0) != WAIT_OBJECT_0 &&
           RemainingMsec(retry_deadline) > 0) {
      ++attempt;
      if (!SleepWithinDeadline(kColdStartRetryMsec, retry_deadline,
                               cancellation_event)) {
        break;
      }

      pipe = TryOpenPipeOnce(pipe_name);
      if (pipe != INVALID_HANDLE_VALUE) {
        ZenzPipeDebugOutput(
            std::wstring(L"CreateFileW succeeded after cold start retry attempt=")
                .append(std::to_wstring(attempt)));
        return pipe;
      }

      const DWORD retry_error = ::GetLastError();
      if (retry_error == ERROR_PIPE_BUSY) {
        const DWORD wait_msec =
            std::min<DWORD>(kColdStartRetryMsec,
                            RemainingMsec(retry_deadline));
        if (wait_msec > 0) {
          ::WaitNamedPipeW(pipe_name.c_str(), wait_msec);
        }
        pipe = TryOpenPipeOnce(pipe_name);
        if (pipe != INVALID_HANDLE_VALUE) {
          ZenzPipeDebugOutput(
              std::wstring(
                  L"CreateFileW succeeded after cold start busy retry attempt=")
                  .append(std::to_wstring(attempt)));
          return pipe;
        }
      }

      if (attempt == 1 || RemainingMsec(retry_deadline) == 0) {
        ZenzPipeDebugOutput(
            std::wstring(L"CreateFileW cold start retry failed attempt=")
                .append(std::to_wstring(attempt))
                .append(L" error=")
                .append(std::to_wstring(retry_error)));
      }
    }

    ZenzPipeDebugOutput(
        std::wstring(L"CreateFileW cold start retry exhausted budget_msec=")
            .append(std::to_wstring(retry_budget_msec)));
    return INVALID_HANDLE_VALUE;
  }

  if (error == ERROR_PIPE_BUSY) {
    constexpr int kBusyRetryAttempts = 3;
    constexpr DWORD kBusyRetryMsec = 20;

    for (int attempt = 0; attempt < kBusyRetryAttempts; ++attempt) {
      const DWORD wait_msec =
          std::min<DWORD>(kBusyRetryMsec,
                          RemainingMsec(transport_deadline));
      if (wait_msec == 0 ||
          ::WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
        break;
      }
      ::WaitNamedPipeW(pipe_name.c_str(), wait_msec);

      ZenzPipeDebugOutput(
          std::wstring(L"CreateFileW retry attempt=")
              .append(std::to_wstring(attempt + 1)));

      pipe = TryOpenPipeOnce(pipe_name);
      if (pipe != INVALID_HANDLE_VALUE) {
        ZenzPipeDebugOutput(L"CreateFileW succeeded after retry");
        return pipe;
      }
    }
  }

  return INVALID_HANDLE_VALUE;
}


#else  // !_WIN32

// POSIX transport (Linux; also compiled on macOS, where production selects
// ZenzUnixSocketClient through zenz_client_factory.cc).  The endpoint is a
// per-user Unix-domain stream socket owned by mozc_zenz_scorer.

class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd() { Reset(); }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return fd_; }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_;
};

bool SetNonBlockingAndCloseOnExec(int fd) {
  const int status_flags = ::fcntl(fd, F_GETFL, 0);
  if (status_flags < 0 ||
      ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
    return false;
  }
  const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
  return descriptor_flags >= 0 &&
         ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

int RemainingMsec(Deadline deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now());
  if (remaining.count() <= 0) {
    return 0;
  }
  return static_cast<int>(std::min<int64_t>(
      std::max<int64_t>(1, remaining.count()),
      std::numeric_limits<int>::max()));
}

// |cancellation_fd| becomes readable once ZenzClient::RequestStop() fires.
bool IsCancelled(int cancellation_fd) {
  pollfd descriptor = {};
  descriptor.fd = cancellation_fd;
  descriptor.events = POLLIN;
  while (true) {
    const int result = ::poll(&descriptor, 1, 0);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result != 0;
  }
}

bool SleepWithinDeadline(int requested_msec, Deadline deadline,
                         int cancellation_fd) {
  const int remaining = RemainingMsec(deadline);
  if (remaining == 0) {
    return false;
  }
  const Deadline wake =
      Clock::now() +
      std::chrono::milliseconds(std::min(requested_msec, remaining));
  while (true) {
    const int wait_msec = RemainingMsec(wake);
    if (wait_msec == 0) {
      break;
    }
    pollfd descriptor = {};
    descriptor.fd = cancellation_fd;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, wait_msec);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result != 0) {
      return false;
    }
  }
  return RemainingMsec(deadline) > 0;
}

PipeIoResult WaitForSocket(int fd, short events, Deadline deadline,
                           int cancellation_fd) {
  while (true) {
    const int remaining = RemainingMsec(deadline);
    if (remaining == 0) {
      return PipeIoResult::kTimeout;
    }
    pollfd descriptors[2] = {};
    descriptors[0].fd = fd;
    descriptors[0].events = events;
    descriptors[1].fd = cancellation_fd;
    descriptors[1].events = POLLIN;
    const int result = ::poll(descriptors, 2, remaining);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PipeIoResult::kError;
    }
    if (result == 0) {
      return PipeIoResult::kTimeout;
    }
    if (descriptors[1].revents != 0) {
      return PipeIoResult::kError;
    }
    // POLLHUP/POLLERR are reported by the following send()/recv().
    return PipeIoResult::kOk;
  }
}

PipeIoResult WriteAllUntil(int handle, const void* data, uint32_t size,
                           Deadline deadline, int cancellation_fd) {
#if defined(__APPLE__)
  constexpr int kSendFlags = MSG_DONTWAIT;
#else
  constexpr int kSendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#endif
  const uint8_t* ptr = static_cast<const uint8_t*>(data);
  uint32_t remaining = size;

  while (remaining > 0) {
    const PipeIoResult ready =
        WaitForSocket(handle, POLLOUT, deadline, cancellation_fd);
    if (ready != PipeIoResult::kOk) {
      return ready;
    }
    const ssize_t written = ::send(handle, ptr, remaining, kSendFlags);
    if (written < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return PipeIoResult::kError;
    }
    if (written == 0) {
      return PipeIoResult::kError;
    }
    ptr += written;
    remaining -= static_cast<uint32_t>(written);
  }

  return PipeIoResult::kOk;
}

PipeIoResult ReadAllUntil(int handle, void* data, uint32_t size,
                          Deadline deadline, int cancellation_fd) {
  uint8_t* ptr = static_cast<uint8_t*>(data);
  uint32_t remaining = size;

  while (remaining > 0) {
    const PipeIoResult ready =
        WaitForSocket(handle, POLLIN, deadline, cancellation_fd);
    if (ready != PipeIoResult::kOk) {
      return ready;
    }
    const ssize_t read_bytes = ::recv(handle, ptr, remaining, MSG_DONTWAIT);
    if (read_bytes < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return PipeIoResult::kError;
    }
    if (read_bytes == 0) {
      return PipeIoResult::kError;
    }
    ptr += read_bytes;
    remaining -= static_cast<uint32_t>(read_bytes);
  }

  return PipeIoResult::kOk;
}

int TryOpenPipeOnce(const std::string& pipe_name) {
  int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    return -1;
  }
  const int descriptor_flags = ::fcntl(sock, F_GETFD, 0);
  if (descriptor_flags < 0 ||
      ::fcntl(sock, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    ::close(sock);
    return -1;
  }
#if defined(__APPLE__)
  const int no_sigpipe = 1;
  if (::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                   sizeof(no_sigpipe)) != 0) {
    ::close(sock);
    return -1;
  }
#endif

  struct stat socket_info = {};
  if (::lstat(pipe_name.c_str(), &socket_info) != 0) {
    const int error = errno;
    ::close(sock);
    errno = error;
    return -1;
  }
  if (!S_ISSOCK(socket_info.st_mode) ||
      socket_info.st_uid != ::geteuid() ||
      (socket_info.st_mode & 0077) != 0) {
    ::close(sock);
    errno = EACCES;
    return -1;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if (pipe_name.size() >= sizeof(addr.sun_path)) {
    ::close(sock);
    return -1;
  }
  std::strncpy(addr.sun_path, pipe_name.c_str(), sizeof(addr.sun_path) - 1);
  const socklen_t addr_size = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + pipe_name.size() + 1);
#if defined(__APPLE__)
  addr.sun_len = static_cast<unsigned char>(addr_size);
#endif

  if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                addr_size) < 0) {
    const int error = errno;
    ::close(sock);
    errno = error;
    return -1;
  }

  return sock;
}

bool LaunchZenzScorerIfNeeded() {
  static std::atomic<uint64_t> last_launch_tick{0};
  constexpr uint64_t kLaunchThrottleMsec = 2000;

  const uint64_t now = absl::ToUnixMillis(absl::Now());
  uint64_t previous = last_launch_tick.load();

  if (previous != 0 && now - previous < kLaunchThrottleMsec) {
    return false;
  }

  while (!last_launch_tick.compare_exchange_weak(previous, now)) {
    if (previous != 0 && now - previous < kLaunchThrottleMsec) {
      return false;
    }
  }

  char exe_path[4096] = {};
#if defined(__APPLE__)
  uint32_t executable_path_size = sizeof(exe_path);
  const ssize_t len =
      ::_NSGetExecutablePath(exe_path, &executable_path_size) == 0
          ? static_cast<ssize_t>(std::strlen(exe_path))
          : -1;
#else
  const ssize_t len =
      ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
#endif
  std::string dir = ".";
  if (len > 0) {
    exe_path[len] = '\0';
    std::string path(exe_path);
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
      dir = path.substr(0, pos);
    }
  }

#if defined(__APPLE__)
  std::string scorer_path = dir + "/../Resources/mozc_zenz_scorer";
#else
  std::string scorer_path = dir + "/mozc_zenz_scorer";
#endif
  if (::access(scorer_path.c_str(), X_OK) != 0) {
    return false;
  }

  pid_t pid = ::fork();
  if (pid == 0) {
    // Child process: double fork to avoid zombies
    pid_t pid2 = ::fork();
    if (pid2 == 0) {
      if (::setsid() < 0) {
        ::_exit(127);
      }

      // Redirect stdio to /dev/null
      int fd = ::open("/dev/null", O_RDWR);
      if (fd >= 0) {
        ::dup2(fd, STDIN_FILENO);
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > 2) {
          ::close(fd);
        }
      }

      ::execl(scorer_path.c_str(), "mozc_zenz_scorer", nullptr);
      ::_exit(127);
    }
    ::_exit(0);
  } else if (pid > 0) {
    // Parent process: wait for the first child
    ::waitpid(pid, nullptr, 0);
    return true;
  }

  return false;
}

int OpenPipeWithAutoLaunch(const std::string& pipe_name,
                           uint32_t request_timeout_msec,
                           Deadline transport_deadline,
                           int cancellation_fd) {
  if (IsCancelled(cancellation_fd)) {
    return -1;
  }

  int pipe = TryOpenPipeOnce(pipe_name);
  if (pipe >= 0) {
    return pipe;
  }

  const int error = errno;
  if (error == ENOENT || error == ECONNREFUSED) {
    LaunchZenzScorerIfNeeded();

    constexpr int kColdStartRetryMsec = 50;
    const uint32_t retry_budget_msec =
        std::max<uint32_t>(request_timeout_msec, kColdStartRetryMsec);
    const Deadline retry_deadline = std::min(
        transport_deadline,
        Clock::now() + std::chrono::milliseconds(retry_budget_msec));

    while (SleepWithinDeadline(kColdStartRetryMsec, retry_deadline,
                               cancellation_fd)) {
      pipe = TryOpenPipeOnce(pipe_name);
      if (pipe >= 0) {
        return pipe;
      }
    }
  }

  return -1;
}

#endif  // _WIN32

}  // namespace

ZenzNamedPipeClient::ZenzNamedPipeClient(
    uint32_t transport_safety_timeout_msec)
    : transport_safety_timeout_msec_(
          std::max<uint32_t>(1, transport_safety_timeout_msec)) {}

#if defined(_WIN32)
ZenzNamedPipeClient::ZenzNamedPipeClient(
    uint32_t transport_safety_timeout_msec,
    PeerVerifierForTesting verifier_for_testing)
    : ZenzNamedPipeClient(transport_safety_timeout_msec) {
  verifier_for_testing_ = verifier_for_testing;
}
#endif

bool ZenzNamedPipeClient::IsAvailable() const {
  // Windows named pipe and the Mozkey IbG POSIX (Linux) socket transport.
  return true;
}

ZenzLiveResponse ZenzNamedPipeClient::Convert(
    const ZenzLiveRequest& request) {
  ZenzLiveResponse response;
  response.generation = request.generation;
  response.key = request.key;

  if (request.backend_device.size() > kMaxBackendDeviceBytes) {
    response.ok = false;
    response.debug = "backend_device_too_large";
    return response;
  }

#if defined(_WIN32)
  ZenzPipeDebugOutput(
      std::wstring(L"Convert entered generation=")
          .append(std::to_wstring(request.generation))
          .append(L" ")
          .append(RedactedStatsWide(L"key", request.key.size()))
          .append(L" ")
          .append(RedactedStatsWide(L"prompt", request.prompt.size()))
          .append(L" timeout_msec=")
          .append(std::to_wstring(request.timeout_msec)));

  ScopedWinHandle cancellation_event(
      ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (cancellation_event.get() == nullptr) {
    response.debug = "pipe_cancellation_event_failed";
    return response;
  }
  const HANDLE cancellation = cancellation_event.get();
  ScopedInterrupt interrupt(*this,
                            [&] { ::SetEvent(cancellation_event.get()); });
#else
  // Self-pipe used as the cancellation event: RequestStop() writes one byte,
  // which wakes every poll() in this conversion.
  int cancellation_pipe[2] = {-1, -1};
  if (::pipe(cancellation_pipe) != 0) {
    response.debug = "pipe_cancellation_event_failed";
    return response;
  }
  ScopedFd cancellation_read(cancellation_pipe[0]);
  ScopedFd cancellation_write(cancellation_pipe[1]);
  if (!SetNonBlockingAndCloseOnExec(cancellation_read.get()) ||
      !SetNonBlockingAndCloseOnExec(cancellation_write.get())) {
    response.debug = "pipe_cancellation_event_failed";
    return response;
  }
  const int cancellation = cancellation_read.get();
  ScopedInterrupt interrupt(*this, [&] {
    const char byte = 0;
    while (::write(cancellation_write.get(), &byte, 1) < 0 &&
           errno == EINTR) {
    }
  });
#endif  // _WIN32

  const absl::Time start = absl::Now();
  const Deadline transport_deadline =
      Clock::now() +
      std::chrono::milliseconds(transport_safety_timeout_msec_);

  if (request.prompt.size() > kMaxPromptBytes) {
    response.ok = false;
    response.debug = "prompt_too_large";
    return response;
  }

#if defined(_WIN32)
  const std::wstring pipe_name = Utf8ToWidePipeName(request.pipe_name);
  if (pipe_name.empty()) {
    response.ok = false;
    response.debug = "invalid_pipe_name";
    return response;
  }

  ScopedWinHandle pipe(OpenPipeWithAutoLaunch(pipe_name, request.timeout_msec,
                                              transport_deadline,
                                              cancellation));
  const bool pipe_opened = pipe.get() != INVALID_HANDLE_VALUE;
#else
  std::string pipe_name = request.pipe_name;
  if (pipe_name.rfind("\\\\.\\pipe\\", 0) == 0) {
    // The configured default is the Windows pipe name.  POSIX builds map it to
    // the product-specific per-user scorer socket.
    const char* home = ::getenv("HOME");
    pipe_name = std::string(home != nullptr ? home : "/tmp") +
#if !defined(GOOGLE_JAPANESE_INPUT_BUILD)
                "/.mozkey-ibg_zenz_scorer_pipe";
#else
                "/.mozc_zenz_scorer_pipe";
#endif
  }
  if (pipe_name.empty()) {
    response.ok = false;
    response.debug = "invalid_pipe_name";
    return response;
  }

  ScopedFd pipe(OpenPipeWithAutoLaunch(pipe_name, request.timeout_msec,
                                       transport_deadline, cancellation));
  const bool pipe_opened = pipe.get() >= 0;
#endif  // _WIN32

  if (!pipe_opened) {
    response.ok = false;
    response.timeout = RemainingMsec(transport_deadline) == 0;
    response.debug =
        response.timeout ? "pipe_open_timeout" : "pipe_open_failed";
    return response;
  }

#if defined(_WIN32)
  // Authenticate the bound server endpoint before sending a header or a
  // single byte of the user's input. Never retry against an untrusted pipe.
  const bool trusted = verifier_for_testing_ != nullptr
                           ? verifier_for_testing_(pipe.get())
                           : IsTrustedScorerPipeServer(pipe.get());
  if (!trusted) {
    ZenzPipeDebugOutput(L"pipe server identity verification failed");
    response.debug = "pipe_peer_untrusted";
    return response;
  }
#endif  // _WIN32

  ZenzWireRequestHeader request_header = {};
  request_header.magic = kZenzWireMagic;
  request_header.version = kZenzWireVersion;
  request_header.kind = kZenzWireKindRequest;
  request_header.generation = request.generation;
  request_header.timeout_msec = request.timeout_msec;
  request_header.max_output_chars = request.max_output_chars;
  request_header.prompt_size = static_cast<uint32_t>(request.prompt.size());
  request_header.backend_device_size =
      static_cast<uint32_t>(request.backend_device.size());

  PipeIoResult io =
      WriteAllUntil(pipe.get(), &request_header, sizeof(request_header),
                    transport_deadline, cancellation);
  if (io == PipeIoResult::kOk && !request.prompt.empty()) {
    io = WriteAllUntil(pipe.get(), request.prompt.data(),
                       static_cast<uint32_t>(request.prompt.size()),
                       transport_deadline, cancellation);
  }
  if (io == PipeIoResult::kOk && !request.backend_device.empty()) {
    io = WriteAllUntil(pipe.get(), request.backend_device.data(),
                       static_cast<uint32_t>(request.backend_device.size()),
                       transport_deadline, cancellation);
  }

  if (io != PipeIoResult::kOk) {
    response.ok = false;
    response.timeout = io == PipeIoResult::kTimeout;
    response.debug =
        response.timeout ? "pipe_write_timeout" : "pipe_write_failed";
    return response;
  }

  ZenzWireResponseHeader response_header = {};
  io = ReadAllUntil(pipe.get(), &response_header, sizeof(response_header),
                    transport_deadline, cancellation);
  if (io != PipeIoResult::kOk) {
    response.ok = false;
    response.timeout = io == PipeIoResult::kTimeout;
    response.debug =
        response.timeout ? "pipe_read_header_timeout"
                         : "pipe_read_header_failed";
    return response;
  }

  if (response_header.magic != kZenzWireMagic ||
      response_header.version != kZenzWireVersion ||
      response_header.kind != kZenzWireKindResponse ||
      response_header.generation != request.generation) {
    response.ok = false;
    response.debug = "pipe_response_header_invalid";
    return response;
  }
  const uint64_t response_payload_size =
      static_cast<uint64_t>(response_header.value_size) +
      static_cast<uint64_t>(response_header.debug_size);
  if (response_header.value_size > kMaxResponseValueBytes ||
      response_header.debug_size > kMaxResponseDebugBytes ||
      response_payload_size > kMaxWirePayloadBytes) {
    response.ok = false;
    response.debug = "pipe_response_payload_too_large";
    return response;
  }

  std::string value(response_header.value_size, '\0');
  if (response_header.value_size > 0) {
    io = ReadAllUntil(pipe.get(), value.data(), response_header.value_size,
                      transport_deadline, cancellation);
  }

  std::string debug(response_header.debug_size, '\0');
  if (io == PipeIoResult::kOk && response_header.debug_size > 0) {
    io = ReadAllUntil(pipe.get(), debug.data(), response_header.debug_size,
                      transport_deadline, cancellation);
  }

  if (io != PipeIoResult::kOk) {
    response.ok = false;
    response.timeout = io == PipeIoResult::kTimeout;
    response.debug =
        response.timeout ? "pipe_read_payload_timeout"
                         : "pipe_read_payload_failed";
    return response;
  }

#if defined(_WIN32)
  ZenzPipeDebugOutput(
      std::wstring(L"Convert response status=")
          .append(std::to_wstring(response_header.status))
          .append(L" latency_msec=")
          .append(std::to_wstring(response_header.latency_msec))
          .append(L" ")
          .append(RedactedStatsWide(L"value", value.size()))
          .append(L" ")
          .append(RedactedStatsWide(L"debug", debug.size())));
#endif

  response.ok = response_header.status == kZenzWireStatusOk;
  response.timeout = response_header.status == kZenzWireStatusTimeout;
  response.value = std::move(value);
  response.debug = std::move(debug);
  response.latency = absl::Now() - start;
  return response;
}

}  // namespace session
}  // namespace mozc
