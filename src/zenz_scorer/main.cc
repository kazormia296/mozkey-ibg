#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#endif
#include <cstring>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "session/zenz_diagnostic.h"
#include "session/zenz_named_pipe_endpoint.h"
#include "zenz_scorer/json_parser.h"

#if defined(_WIN32)
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace {

std::atomic<bool> g_llama_launch_started{false};
std::atomic<bool> g_llama_ready_probe_started{false};
std::atomic<bool> g_llama_server_ready{false};
std::atomic<bool> g_shutdown_requested{false};
#if !defined(_WIN32)
volatile sig_atomic_t g_posix_shutdown_requested = 0;
#endif

constexpr int kRandomPortMin = 49152;
constexpr int kRandomPortMax = 65535;

#if defined(_WIN32)
class LoopbackPortReservation {
 public:
  LoopbackPortReservation() = default;
  LoopbackPortReservation(const LoopbackPortReservation&) = delete;
  LoopbackPortReservation& operator=(const LoopbackPortReservation&) = delete;
  ~LoopbackPortReservation() { Close(); }

  bool Reserve(int requested_port) {
    if (requested_port < kRandomPortMin || requested_port > kRandomPortMax) {
      return false;
    }

    WSADATA data = {};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return false;
    }
    winsock_started_ = true;

    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
      Close();
      return false;
    }

    BOOL exclusive = TRUE;
    if (::setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive),
                     sizeof(exclusive)) != 0) {
      Close();
      return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(requested_port));
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(socket_, 1) != 0) {
      Close();
      return false;
    }

    sockaddr_in bound = {};
    int bound_size = sizeof(bound);
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&bound),
                      &bound_size) != 0 ||
        ntohs(bound.sin_port) != requested_port) {
      Close();
      return false;
    }
    return true;
  }

  void Close() {
    if (socket_ != INVALID_SOCKET) {
      ::closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    if (winsock_started_) {
      ::WSACleanup();
      winsock_started_ = false;
    }
  }

 private:
  SOCKET socket_ = INVALID_SOCKET;
  bool winsock_started_ = false;
};

std::wstring NormalizeWindowsPath(std::wstring path) {
  if (path.rfind(L"\\\\?\\", 0) == 0) {
    path.erase(0, 4);
  }
  std::transform(path.begin(), path.end(), path.begin(), [](wchar_t value) {
    return static_cast<wchar_t>(::towlower(value));
  });
  return path;
}

bool IsExpectedLlamaProcessImage(HANDLE process,
                                 const std::wstring& expected_path) {
  std::vector<wchar_t> actual_path(32768, L'\0');
  DWORD actual_size = static_cast<DWORD>(actual_path.size());
  if (!::QueryFullProcessImageNameW(process, 0, actual_path.data(),
                                    &actual_size)) {
    return false;
  }

  std::vector<wchar_t> full_expected_path(32768, L'\0');
  DWORD expected_size = ::GetFullPathNameW(
      expected_path.c_str(), static_cast<DWORD>(full_expected_path.size()),
      full_expected_path.data(), nullptr);
  if (expected_size == 0 || expected_size >= full_expected_path.size()) {
    return false;
  }
  full_expected_path.resize(expected_size);
  actual_path.resize(actual_size);
  const std::wstring actual_path_string(actual_path.begin(), actual_path.end());
  const std::wstring expected_path_string(full_expected_path.begin(),
                                          full_expected_path.end());
  return NormalizeWindowsPath(actual_path_string) ==
         NormalizeWindowsPath(expected_path_string);
}
#else
class LoopbackPortReservation {
 public:
  LoopbackPortReservation() = default;
  LoopbackPortReservation(const LoopbackPortReservation&) = delete;
  LoopbackPortReservation& operator=(const LoopbackPortReservation&) = delete;
  ~LoopbackPortReservation() { Close(); }

  bool Reserve(int requested_port) {
    if (requested_port < kRandomPortMin || requested_port > kRandomPortMax) {
      return false;
    }

    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
      return false;
    }
    const int flags = ::fcntl(socket_, F_GETFD, 0);
    if (flags < 0 || ::fcntl(socket_, F_SETFD, flags | FD_CLOEXEC) != 0) {
      Close();
      return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(requested_port));
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(socket_, 1) != 0) {
      Close();
      return false;
    }

    sockaddr_in bound = {};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&bound),
                      &bound_size) != 0 ||
        ntohs(bound.sin_port) != requested_port) {
      Close();
      return false;
    }
    return true;
  }

  void Close() {
    if (socket_ >= 0) {
      ::close(socket_);
      socket_ = -1;
    }
  }

 private:
  int socket_ = -1;
};

std::string CanonicalPath(const std::string& path) {
  char resolved[PATH_MAX] = {};
  if (::realpath(path.c_str(), resolved) == nullptr) {
    return {};
  }
  return resolved;
}

bool IsExpectedLlamaProcessImage(pid_t pid, const std::string& expected_path) {
  char actual_path[PATH_MAX] = {};
#if defined(__linux__)
  char proc_exe[64] = {};
  std::snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", pid);
  const ssize_t size = ::readlink(proc_exe, actual_path, sizeof(actual_path) - 1);
  if (size <= 0) {
    return false;
  }
  actual_path[size] = '\0';
#elif defined(__APPLE__)
  if (::proc_pidpath(pid, actual_path, sizeof(actual_path)) <= 0) {
    return false;
  }
#else
  return true;
#endif

  const std::string expected = CanonicalPath(expected_path);
  const std::string actual = CanonicalPath(actual_path);
  return !expected.empty() && !actual.empty() && expected == actual;
}

bool WaitForExpectedLlamaProcessImage(pid_t pid,
                                      const std::string& expected_path) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (IsExpectedLlamaProcessImage(pid, expected_path)) {
      return true;
    }

    int status = 0;
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno != EINTR)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
#endif
std::atomic<uint64_t> g_llama_runtime_generation{0};

std::mutex g_llama_process_mutex;
std::mutex g_backend_device_mutex;
std::optional<std::string> g_requested_backend_device;
std::string g_effective_backend_device;
#if defined(_WIN32)
HANDLE g_llama_process = nullptr;
HANDLE g_llama_job = nullptr;

constexpr wchar_t kSingleInstanceMutexName[] =
    LR"(Local\MozkeyIbGZenzScorerSingleInstance)";
#else
pid_t g_llama_process = -1;
#if !defined(GOOGLE_JAPANESE_INPUT_BUILD)
constexpr char kDefaultPipeNameSuffix[] = "/.mozkey-ibg_zenz_scorer_pipe";
#else
constexpr char kDefaultPipeNameSuffix[] = "/.mozc_zenz_scorer_pipe";
#endif
#endif

constexpr wchar_t kDefaultHost[] = L"127.0.0.1";
constexpr int kApiKeyBytes = 32;
constexpr int kDefaultCtx = 256;
constexpr int kDefaultThreads = 4;
constexpr int kDefaultNPredict = 64;
constexpr char kDefaultFlashAttention[] = "auto";

constexpr uint32_t kZenzWireMagic = 0x315A4E5A;  // "ZNZ1"
constexpr uint16_t kZenzWireVersion = 2;
constexpr uint16_t kZenzWireKindRequest = 1;
constexpr uint16_t kZenzWireKindResponse = 2;

constexpr uint32_t kStatusOk = 0;
constexpr uint32_t kStatusError = 1;
constexpr uint32_t kStatusTimeout = 2;

// Minimum wait budget used only while llama-server is still loading.
// The actual completion request still uses the request timeout from Mozc.
constexpr uint32_t kMinLlamaReadyWaitMsec = 1500;

// Hard caps for the named-pipe protocol.  The pipe is restricted to the current
// user, but the scorer still must not trust client-provided lengths/timeouts.
constexpr uint32_t kMaxPromptBytes = 8192;
constexpr uint32_t kMaxOutputChars = 256;
constexpr uint32_t kMaxRequestTimeoutMsec = 5000;
// The readiness probe performs the same small completion used to prove that
// llama-server is usable.  Give it the full request budget: a shorter probe
// can permanently keep the scorer in server_loading when inference succeeds
// within the client SLA but takes longer than the probe-only timeout.
constexpr uint32_t kLlamaReadyProbeTimeoutMsec = kMaxRequestTimeoutMsec;
constexpr size_t kMaxHttpResponseBytes = 65536;
constexpr uint32_t kMaxBackendDeviceBytes = 128;

// Hard caps for environment-controlled runtime knobs.
constexpr int kMaxCtx = 1024;
constexpr int kMaxThreads = 16;
constexpr int kMaxNPredict = 256;

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

struct ZenzWireResponseHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t kind;
  uint32_t generation;
  uint32_t status;
  uint32_t latency_msec;
  uint32_t value_size;
  uint32_t debug_size;
};
#pragma pack(pop)

#if defined(_WIN32)
struct Options {
  std::wstring pipe_name;
  std::wstring host = kDefaultHost;
  int port = 0;
  std::string api_key;
  bool random_ok = false;

  int ctx = kDefaultCtx;
  int threads = kDefaultThreads;
  int n_predict = kDefaultNPredict;
  std::string flash_attention = kDefaultFlashAttention;
  std::string backend_device;

  std::wstring llama_server_path;
  std::wstring model_path;
};
#else
struct Options {
  std::string pipe_name;
  std::string host = "127.0.0.1";
  int port = 0;
  std::string api_key;
  bool random_ok = false;

  int ctx = kDefaultCtx;
  int threads = kDefaultThreads;
  int n_predict = kDefaultNPredict;
  std::string flash_attention = kDefaultFlashAttention;
  std::string backend_device;

  std::string llama_server_path;
  std::string model_path;
};
#endif

#if defined(_WIN32)
void Debug(const std::wstring& message) {
  std::wstring line = L"[mozc-zenz-scorer] ";
  line.append(message);
  line.push_back(L'\n');
  OutputDebugStringW(line.c_str());

  std::wcerr << line;
}

std::wstring RedactedBytes(const wchar_t* label, size_t bytes) {
  std::wstring output(label);
  output.append(L"_bytes=");
  output.append(std::to_wstring(bytes));
  return output;
}

std::wstring RedactedWideChars(const wchar_t* label,
                               const std::wstring& text) {
  std::wstring output(label);
  output.append(L"_chars=");
  output.append(std::to_wstring(text.size()));
  return output;
}

std::wstring RedactedUtf8Bytes(const wchar_t* label,
                               const std::string& text) {
  return RedactedBytes(label, text.size());
}

std::wstring Utf8ToWide(const std::string& input) {
  if (input.empty()) {
    return L"";
  }

  const int size = MultiByteToWideChar(
      CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (size <= 0) {
    return L"<invalid utf8>";
  }

  std::wstring output(size, L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
      output.data(), size);
  return output;
}

std::string WideToUtf8(const std::wstring& input) {
  if (input.empty()) {
    return "";
  }

  const int size = WideCharToMultiByte(
      CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return "";
  }

  std::string output(size, '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
      output.data(), size, nullptr, nullptr);
  return output;
}

std::wstring GetEnvWide(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0) {
    return L"";
  }

  std::wstring value(size, L'\0');
  DWORD written = GetEnvironmentVariableW(name, value.data(), size);
  if (written == 0) {
    return L"";
  }

  value.resize(written);
  return value;
}

int GetEnvInt(const wchar_t* name, int default_value) {
  std::wstring value = GetEnvWide(name);
  if (value.empty()) {
    return default_value;
  }

  wchar_t* end = nullptr;
  const long parsed = std::wcstol(value.c_str(), &end, 10);
  if (end == value.c_str() || parsed <= 0) {
    return default_value;
  }

  return static_cast<int>(parsed);
}

bool FillRandomBytes(void* buffer, size_t size) {
  if (buffer == nullptr) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  if (size > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
    return false;
  }

  return ::BCryptGenRandom(
             nullptr,
             static_cast<PUCHAR>(buffer),
             static_cast<ULONG>(size),
             BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string HexEncode(const uint8_t* data, size_t size) {
  constexpr char kHex[] = "0123456789abcdef";

  std::string output;
  output.reserve(size * 2);

  for (size_t i = 0; i < size; ++i) {
    const uint8_t b = data[i];
    output.push_back(kHex[(b >> 4) & 0x0f]);
    output.push_back(kHex[b & 0x0f]);
  }

  return output;
}

int GenerateRandomPort() {
  uint32_t value = 0;
  if (!FillRandomBytes(&value, sizeof(value))) {
    return 0;
  }

  constexpr int kRange = kRandomPortMax - kRandomPortMin + 1;
  return kRandomPortMin + static_cast<int>(value % kRange);
}

std::string GenerateApiKey() {
  std::vector<uint8_t> bytes(kApiKeyBytes);
  if (!FillRandomBytes(bytes.data(), bytes.size())) {
    return "";
  }

  return HexEncode(bytes.data(), bytes.size());
}

std::wstring GetExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
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

bool FileExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

Options LoadOptions() {
  Options options;
  options.pipe_name =
      Utf8ToWide(mozc::session::kDefaultZenzNamedPipeName);

  const std::wstring exe_dir = GetExeDirectory();

#if !defined(NDEBUG)
  options.llama_server_path = GetEnvWide(L"MOZC_ZENZ_LLAMA_SERVER");
#endif  // !defined(NDEBUG)

  if (options.llama_server_path.empty()) {
    options.llama_server_path = JoinPath(exe_dir, L"llama-server.exe");
  }

#if !defined(NDEBUG)
  options.model_path = GetEnvWide(L"MOZC_ZENZ_MODEL");
#endif  // !defined(NDEBUG)

  if (options.model_path.empty()) {
    options.model_path =
        JoinPath(JoinPath(exe_dir, L"models"), L"zenz-v3.2-small-Q5_K_M.gguf");
  }

  options.port = GenerateRandomPort();
  options.api_key = GenerateApiKey();
  options.random_ok = options.port >= kRandomPortMin &&
                      options.port <= kRandomPortMax &&
                      !options.api_key.empty();

  options.ctx = std::clamp(
      GetEnvInt(L"MOZC_ZENZ_CTX", kDefaultCtx),
      64,
      kMaxCtx);
  options.threads = std::clamp(
      GetEnvInt(L"MOZC_ZENZ_THREADS", kDefaultThreads),
      1,
      kMaxThreads);
  options.n_predict = std::clamp(
      GetEnvInt(L"MOZC_ZENZ_N_PREDICT", kDefaultNPredict),
      4,
      kMaxNPredict);
  options.flash_attention = WideToUtf8(
      GetEnvWide(L"MOZC_ZENZ_FLASH_ATTENTION"));
  if (options.flash_attention.empty()) {
    options.flash_attention = kDefaultFlashAttention;
  }

  return options;
}

#else
void Debug(const std::string& message) {
  std::string line = "[mozc-zenz-scorer] ";
  line.append(message);
  line.push_back('\n');
  std::cerr << line;
}

std::string RedactedBytes(const char* label, size_t bytes) {
  std::string output(label);
  output.append("_bytes=");
  output.append(std::to_string(bytes));
  return output;
}

std::string RedactedWideChars(const char* label, const std::string& text) {
  std::string output(label);
  output.append("_chars=");
  output.append(std::to_string(text.size()));
  return output;
}

std::string RedactedUtf8Bytes(const char* label, const std::string& text) {
  return RedactedBytes(label, text.size());
}

// std::string Utf8ToWide(const std::string& input) { return input; }
// std::string WideToUtf8(const std::string& input) { return input; }

std::string GetEnvString(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : "";
}

int GetEnvInt(const char* name, int default_value) {
  std::string value = GetEnvString(name);
  if (value.empty()) return default_value;
  char* end = nullptr;
  long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || parsed <= 0) return default_value;
  return static_cast<int>(parsed);
}

bool FillRandomBytes(void* buffer, size_t size) {
  if (buffer == nullptr) return false;
  if (size == 0) return true;
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) return false;
  size_t read = std::fread(buffer, 1, size, f);
  std::fclose(f);
  return read == size;
}

int GenerateRandomPort() {
  uint32_t value = 0;
  if (!FillRandomBytes(&value, sizeof(value))) return 0;
  constexpr int kRange = kRandomPortMax - kRandomPortMin + 1;
  return kRandomPortMin + static_cast<int>(value % kRange);
}

std::string GenerateApiKey() {
  std::vector<uint8_t> bytes(kApiKeyBytes);
  if (!FillRandomBytes(bytes.data(), bytes.size())) return "";
  constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    output.push_back(kHex[(b >> 4) & 0x0f]);
    output.push_back(kHex[b & 0x0f]);
  }
  return output;
}

std::string GetExeDirectory() {
  char path[4096] = {};
#if defined(__APPLE__)
  uint32_t executable_path_size = sizeof(path);
  const ssize_t size =
      ::_NSGetExecutablePath(path, &executable_path_size) == 0
          ? static_cast<ssize_t>(std::strlen(path))
          : -1;
#else
  const ssize_t size =
      ::readlink("/proc/self/exe", path, sizeof(path) - 1);
#endif
  if (size <= 0) return ".";
  path[size] = '\0';
  std::string full(path);
  size_t pos = full.find_last_of('/');
  return pos == std::string::npos ? "." : full.substr(0, pos);
}

std::string JoinPath(const std::string& dir, const std::string& file) {
  if (dir.empty()) return file;
  if (dir.back() == '/') return dir + file;
  return dir + "/" + file;
}

bool FileExists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}

Options LoadOptions() {
  Options options;
  const std::string exe_dir = GetExeDirectory();

#if !defined(NDEBUG)
  options.llama_server_path = GetEnvString("MOZC_ZENZ_LLAMA_SERVER");
#endif
  if (options.llama_server_path.empty()) {
    options.llama_server_path = JoinPath(exe_dir, "llama-server");
  }

#if !defined(NDEBUG)
  options.model_path = GetEnvString("MOZC_ZENZ_MODEL");
#endif
  if (options.model_path.empty()) {
    options.model_path = JoinPath(JoinPath(exe_dir, "models"), "zenz-v3.2-small-Q5_K_M.gguf");
  }

  options.port = GenerateRandomPort();
  options.api_key = GenerateApiKey();
  options.random_ok = options.port >= kRandomPortMin && options.port <= kRandomPortMax && !options.api_key.empty();

  options.ctx = std::clamp(GetEnvInt("MOZC_ZENZ_CTX", kDefaultCtx), 64, kMaxCtx);
  options.threads = std::clamp(GetEnvInt("MOZC_ZENZ_THREADS", kDefaultThreads), 1, kMaxThreads);
  options.n_predict = std::clamp(GetEnvInt("MOZC_ZENZ_N_PREDICT", kDefaultNPredict), 4, kMaxNPredict);
  options.flash_attention = GetEnvString("MOZC_ZENZ_FLASH_ATTENTION");
  if (options.flash_attention.empty()) {
    options.flash_attention = kDefaultFlashAttention;
  }

  return options;
}
#endif

bool IsValidFlashAttentionValue(absl::string_view value) {
  return value == "auto" || value == "on" || value == "off";
}

bool IsValidBackendDeviceName(const std::string& name) {
  if (name.size() > kMaxBackendDeviceBytes) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](const unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
  });
}

std::string JsonEscapeUtf8(const std::string& input) {
  std::string output;
  output.reserve(input.size() + 32);

  for (unsigned char c : input) {
    switch (c) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8] = {};
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          output += buf;
        } else {
          output.push_back(static_cast<char>(c));
        }
        break;
    }
  }

  return output;
}

std::string GetDiagnosticEnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

std::string GetDiagnosticFileSha256(const char* environment_name,
                                    absl::string_view path) {
  const std::string override = GetDiagnosticEnvironmentValue(environment_name);
  if (!override.empty()) {
    return override;
  }
  return mozc::session::ZenzDiagnosticCapture::Sha256File(path);
}

class ScorerHttpDiagnosticScope final {
 public:
  ScorerHttpDiagnosticScope(uint32_t generation,
                            std::string request_kind,
                            std::string prompt,
                            std::string http_json,
                            int ctx,
                            int threads,
                            int n_predict,
                            uint32_t max_output_chars,
                            std::string backend_device,
                            std::string flash_attention,
                            std::string runtime_path,
                            std::string model_path,
                            std::string endpoint,
                            std::string* value,
                            std::string* debug)
      : enabled_(mozc::session::ZenzDiagnosticCapture::IsEnabled()),
        generation_(generation),
        request_kind_(std::move(request_kind)),
        value_(value),
        debug_(debug),
        start_(std::chrono::steady_clock::now()) {
    if (!enabled_) {
      return;
    }

    mozc::session::ZenzDiagnosticJson diagnostic;
    diagnostic.AddString("event", "scorer_request");
    diagnostic.AddUint64("generation", generation_);
    diagnostic.AddString("request_kind", request_kind_);
    diagnostic.AddBase64("prompt_base64", prompt);
    diagnostic.AddCodepoints("prompt_codepoints", prompt);
    diagnostic.AddBase64("http_json_base64", http_json);
    diagnostic.AddString("runtime_path", runtime_path);
    diagnostic.AddString("model_path", model_path);
    diagnostic.AddString("endpoint", endpoint);
    diagnostic.AddString(
        "runtime_sha256",
        GetDiagnosticFileSha256("MOZC_ZENZ_RUNTIME_SHA256", runtime_path));
    diagnostic.AddString(
        "model_sha256",
        GetDiagnosticFileSha256("MOZC_ZENZ_MODEL_SHA256", model_path));

    mozc::session::ZenzDiagnosticJson runtime_args;
    runtime_args.AddUint64("ctx", static_cast<uint64_t>(ctx));
    runtime_args.AddUint64("threads", static_cast<uint64_t>(threads));
    runtime_args.AddUint64("n_predict", static_cast<uint64_t>(n_predict));
    runtime_args.AddUint64("max_output_chars", max_output_chars);
    runtime_args.AddString("device", backend_device.empty()
                                          ? "auto"
                                          : backend_device);
    runtime_args.AddString("flash_attention", flash_attention);
    diagnostic.AddObject("runtime_args", runtime_args.Build());
    mozc::session::ZenzDiagnosticCapture::Write(diagnostic);
  }

  ScorerHttpDiagnosticScope(const ScorerHttpDiagnosticScope&) = delete;
  ScorerHttpDiagnosticScope& operator=(const ScorerHttpDiagnosticScope&) =
      delete;

  ~ScorerHttpDiagnosticScope() {
    if (!enabled_) {
      return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_);
    mozc::session::ZenzDiagnosticJson diagnostic;
    diagnostic.AddString("event", "scorer_response");
    diagnostic.AddUint64("generation", generation_);
    diagnostic.AddString("request_kind", request_kind_);
    diagnostic.AddBool("ok", ok_);
    diagnostic.AddString("debug", debug_ == nullptr ? "" : *debug_);
    diagnostic.AddUint64("latency_msec",
                         static_cast<uint64_t>(std::max<int64_t>(
                             0, elapsed.count())));
    diagnostic.AddBase64("raw_http_response_body_base64",
                         raw_http_response_body_);
    diagnostic.AddBase64("raw_model_output_base64", raw_model_output_);
    diagnostic.AddBase64("clean_generated_text_base64",
                         clean_generated_text_);
    if (value_ != nullptr) {
      diagnostic.AddBase64("value_base64", *value_);
    }
    mozc::session::ZenzDiagnosticCapture::Write(diagnostic);
  }

  void SetRawHttpResponseBody(std::string body) {
    if (enabled_) {
      raw_http_response_body_ = std::move(body);
    }
  }

  void SetRawModelOutput(std::string output) {
    if (enabled_) {
      raw_model_output_ = std::move(output);
    }
  }

  void SetCleanGeneratedText(std::string output) {
    if (enabled_) {
      clean_generated_text_ = std::move(output);
    }
  }

  void MarkSuccess() {
    if (enabled_) {
      ok_ = true;
    }
  }

 private:
  const bool enabled_;
  const uint32_t generation_;
  const std::string request_kind_;
  std::string* const value_;
  std::string* const debug_;
  const std::chrono::steady_clock::time_point start_;

  bool ok_ = false;
  std::string raw_http_response_body_;
  std::string raw_model_output_;
  std::string clean_generated_text_;
};

std::string TrimAsciiWhitespace(std::string s) {
  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.front());
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      break;
    }
    s.erase(s.begin());
  }

  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.back());
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      break;
    }
    s.pop_back();
  }

  return s;
}

std::string TruncateUtf8ByChars(const std::string& input, uint32_t max_chars) {
  if (max_chars == 0) {
    return "";
  }

  size_t pos = 0;
  uint32_t count = 0;

  while (pos < input.size() && count < max_chars) {
    const unsigned char c = static_cast<unsigned char>(input[pos]);
    size_t char_len = 1;

    if ((c & 0x80) == 0) {
      char_len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      char_len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      char_len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      char_len = 4;
    } else {
      break;
    }

    if (pos + char_len > input.size()) {
      break;
    }

    pos += char_len;
    ++count;
  }

  return input.substr(0, pos);
}

std::string CleanGeneratedText(std::string text, uint32_t max_output_chars) {
  const std::vector<std::string> stop_markers = {
      "\xEE\xB8\x80",  // U+EE00
      "\xEE\xB8\x81",  // U+EE01
      "\xEE\xB8\x82",  // U+EE02
      "\xEE\xB8\x83",  // U+EE03
      "\xEE\xB8\x84",  // U+EE04
      "\xEE\xB8\x85",  // U+EE05
      "\xEE\xB8\x86",  // U+EE06
      "\xEE\xB8\x87",  // U+EE07
      "\xEE\xB8\x88",  // U+EE08
      "\xEE\xB8\x89",  // U+EE09
      "\xEE\xB8\x8A",  // U+EE0A
      "\xEE\xB8\x8B",  // U+EE0B
      "\xEE\xB8\x8C",  // U+EE0C
      "\xEE\xB8\x8D",  // U+EE0D
      "\xEE\xB8\x8E",  // U+EE0E
      "\xEE\xB8\x8F",  // U+EE0F
      "<s>",
      "</s>",
      "<unk>",
      "<|endoftext|>",
      "\r",
      "\n",
  };

  size_t end = text.size();
  for (const std::string& marker : stop_markers) {
    const size_t pos = text.find(marker);
    if (pos != std::string::npos) {
      end = std::min(end, pos);
    }
  }

  text = text.substr(0, end);
  text = TrimAsciiWhitespace(text);

  if (max_output_chars > 0) {
    text = TruncateUtf8ByChars(text, max_output_chars);
  }

  return text;
}

#if defined(_WIN32)
bool ReadAll(HANDLE handle, void* data, uint32_t size) {
  uint8_t* ptr = static_cast<uint8_t*>(data);
  uint32_t remaining = size;

  while (remaining > 0) {
    DWORD read = 0;
    if (!ReadFile(handle, ptr, remaining, &read, nullptr)) {
      return false;
    }
    if (read == 0) {
      return false;
    }
    ptr += read;
    remaining -= read;
  }

  return true;
}

bool WriteAll(HANDLE handle, const void* data, uint32_t size) {
  const uint8_t* ptr = static_cast<const uint8_t*>(data);
  uint32_t remaining = size;

  while (remaining > 0) {
    DWORD written = 0;
    if (!WriteFile(handle, ptr, remaining, &written, nullptr)) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    ptr += written;
    remaining -= written;
  }

  return true;
}

bool GetCurrentUserSidString(std::wstring* sid_string) {
  if (sid_string == nullptr) {
    return false;
  }

  sid_string->clear();

  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    Debug(L"OpenProcessToken failed error=" + std::to_wstring(GetLastError()));
    return false;
  }

  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0) {
    Debug(L"GetTokenInformation size failed error=" +
          std::to_wstring(GetLastError()));
    CloseHandle(token);
    return false;
  }

  std::vector<uint8_t> buffer(size);
  TOKEN_USER* token_user = reinterpret_cast<TOKEN_USER*>(buffer.data());
  if (!GetTokenInformation(token, TokenUser, token_user, size, &size)) {
    Debug(L"GetTokenInformation failed error=" +
          std::to_wstring(GetLastError()));
    CloseHandle(token);
    return false;
  }

  LPWSTR raw_sid_string = nullptr;
  if (!ConvertSidToStringSidW(token_user->User.Sid, &raw_sid_string)) {
    Debug(L"ConvertSidToStringSidW failed error=" +
          std::to_wstring(GetLastError()));
    CloseHandle(token);
    return false;
  }

  *sid_string = raw_sid_string;
  LocalFree(raw_sid_string);
  CloseHandle(token);
  return !sid_string->empty();
}

bool BuildCurrentUserOnlyPipeSecurityDescriptor(
    PSECURITY_DESCRIPTOR* security_descriptor) {
  if (security_descriptor == nullptr) {
    return false;
  }

  *security_descriptor = nullptr;

  std::wstring user_sid;
  if (!GetCurrentUserSidString(&user_sid)) {
    return false;
  }

  // Protected DACL:
  //   current user: Generic Read + Generic Write
  //   SYSTEM: Generic All
  //
  // Do not grant Everyone/World access.  Do not add a Low Integrity label.
  const std::wstring sddl =
      L"D:P"
      L"(A;;GRGW;;;" + user_sid + L")"
      L"(A;;GA;;;SY)";

  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(),
          SDDL_REVISION_1,
          security_descriptor,
          nullptr)) {
    Debug(L"ConvertStringSecurityDescriptorToSecurityDescriptorW failed error=" +
          std::to_wstring(GetLastError()));
    *security_descriptor = nullptr;
    return false;
  }

  return true;
}

void ResetLlamaReadyState() {
  g_llama_launch_started = false;
  g_llama_ready_probe_started = false;
  g_llama_server_ready = false;
}

void StopLlamaServer() {
  g_llama_runtime_generation.fetch_add(1);
  std::lock_guard<std::mutex> lock(g_llama_process_mutex);

  if (g_llama_job != nullptr) {
    ::TerminateJobObject(g_llama_job, 0);
  }

  if (g_llama_process != nullptr) {
    ::TerminateProcess(g_llama_process, 0);
    ::WaitForSingleObject(g_llama_process, 3000);
    ::CloseHandle(g_llama_process);
    g_llama_process = nullptr;
  }

  if (g_llama_job != nullptr) {
    ::CloseHandle(g_llama_job);
    g_llama_job = nullptr;
  }

  ResetLlamaReadyState();
}

std::string SelectBackendDevice(const std::string& backend_device) {
  std::lock_guard<std::mutex> lock(g_backend_device_mutex);
  if (!g_requested_backend_device.has_value()) {
    g_requested_backend_device = backend_device;
    g_effective_backend_device = backend_device;
  } else if (*g_requested_backend_device != backend_device) {
    g_requested_backend_device = backend_device;
    g_effective_backend_device = backend_device;
    Debug(L"backend device changed; restarting llama-server");
    StopLlamaServer();
  }
  return g_effective_backend_device;
}

bool LlamaServerExited() {
  std::lock_guard<std::mutex> lock(g_llama_process_mutex);
  return g_llama_process != nullptr &&
         ::WaitForSingleObject(g_llama_process, 0) == WAIT_OBJECT_0;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD control_type) {
  switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_shutdown_requested = true;
      StopLlamaServer();
      return TRUE;
    default:
      return FALSE;
  }
}

void RememberLlamaProcess(PROCESS_INFORMATION* process) {
  if (process == nullptr || process->hProcess == nullptr) {
    return;
  }

  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!::SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &info,
            sizeof(info))) {
      ::CloseHandle(job);
      job = nullptr;
    }
  }

  if (job != nullptr && !::AssignProcessToJobObject(job, process->hProcess)) {
    Debug(L"AssignProcessToJobObject failed error=" +
          std::to_wstring(::GetLastError()));
    ::CloseHandle(job);
    job = nullptr;
  }

  std::lock_guard<std::mutex> lock(g_llama_process_mutex);

  if (g_llama_process != nullptr) {
    ::CloseHandle(g_llama_process);
    g_llama_process = nullptr;
  }

  if (g_llama_job != nullptr) {
    ::CloseHandle(g_llama_job);
    g_llama_job = nullptr;
  }

  g_llama_process = process->hProcess;
  process->hProcess = nullptr;

  g_llama_job = job;
}

bool LaunchLlamaServer(const Options& options, std::wstring* error) {
  if (!FileExists(options.llama_server_path)) {
    *error = L"llama_server_not_found";
    return false;
  }

  if (!FileExists(options.model_path)) {
    *error = L"model_not_found";
    return false;
  }

  // Reserve the exact loopback port until the child has been created.  The
  // llama.cpp server does not currently accept an inherited listening socket,
  // so the reservation is released immediately before exec/CreateProcess and
  // the resulting child is checked before it is trusted.
  LoopbackPortReservation port_reservation;
  if (!port_reservation.Reserve(options.port)) {
    *error = L"loopback_port_reservation_failed";
    return false;
  }

  std::wstring cmd;
  cmd += L"\"";
  cmd += options.llama_server_path;
  cmd += L"\" -m \"";
  cmd += options.model_path;
  cmd += L"\" -c ";
  cmd += std::to_wstring(options.ctx);
  cmd += L" -t ";
  cmd += std::to_wstring(options.threads);
  cmd += L" --host 127.0.0.1 --port ";
  cmd += std::to_wstring(options.port);
  // The API key is defense-in-depth for accidental or stale localhost servers.
  // It is passed to llama-server via command line because llama-server exposes
  // --api-key.  Do not treat it as a strong same-user secret.
  cmd += L" --api-key ";
  cmd += Utf8ToWide(options.api_key);
  cmd += L" --flash-attn ";
  cmd += Utf8ToWide(options.flash_attention);
  if (!options.backend_device.empty()) {
    cmd += L" --device ";
    cmd += Utf8ToWide(options.backend_device);
  }

  Debug(L"launch llama-server port=random api_key_bytes=" +
        std::to_wstring(options.api_key.size()));

  std::vector<wchar_t> cmd_buffer(cmd.begin(), cmd.end());
  cmd_buffer.push_back(L'\0');

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION process = {};

  const std::wstring work_dir = GetExeDirectory();

  if (!CreateProcessW(
          options.llama_server_path.c_str(),
          cmd_buffer.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_NO_WINDOW,
          nullptr,
          work_dir.c_str(),
          &startup,
          &process)) {
    const DWORD err = GetLastError();
    *error = L"CreateProcessW failed error=" + std::to_wstring(err);
    return false;
  }

  port_reservation.Close();
  if (!IsExpectedLlamaProcessImage(process.hProcess,
                                   options.llama_server_path)) {
    ::TerminateProcess(process.hProcess, 1);
    ::WaitForSingleObject(process.hProcess, 3000);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    *error = L"llama_process_identity_invalid";
    return false;
  }

  ::CloseHandle(process.hThread);
  RememberLlamaProcess(&process);

  if (process.hProcess != nullptr) {
    ::CloseHandle(process.hProcess);
  }

  return true;
}

bool HttpPostCompletion(
    const Options& options,
    const std::string& prompt,
    uint32_t timeout_msec,
    uint32_t max_output_chars,
    uint32_t generation,
    const std::string& request_kind,
    std::string* value,
    std::string* debug) {
  value->clear();
  debug->clear();

  HINTERNET session = WinHttpOpen(
      L"mozc_zenz_scorer/1.0",
      WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0);

  if (!session) {
    *debug = "winhttp_open_failed";
    return false;
  }

  const int timeout = static_cast<int>(std::max<uint32_t>(timeout_msec, 50));
  WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

  HINTERNET connect = WinHttpConnect(
      session,
      options.host.c_str(),
      static_cast<INTERNET_PORT>(options.port),
      0);

  if (!connect) {
    WinHttpCloseHandle(session);
    *debug = "winhttp_connect_failed";
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(
      connect,
      L"POST",
      L"/completion",
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      0);

  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    *debug = "winhttp_open_request_failed";
    return false;
  }

  const int requested_n_predict =
      max_output_chars > 0 ? static_cast<int>(max_output_chars)
                          : options.n_predict;
  const int n_predict =
      std::max(4, std::min(options.n_predict, requested_n_predict));

  std::string body;
  body += "{";
  body += "\"prompt\":\"";
  body += JsonEscapeUtf8(prompt);
  body += "\",";
  body += "\"n_predict\":";
  body += std::to_string(n_predict);
  body += ",";
  body += "\"temperature\":0.0,";
  body += "\"top_k\":1,";
  body += "\"top_p\":1.0,";
  body += "\"stream\":false,";
  body += "\"cache_prompt\":true,";
  body += "\"stop\":["
          "\"\\uee00\","
          "\"\\uee01\","
          "\"\\uee02\","
          "\"\\uee03\","
          "\"\\uee04\","
          "\"\\uee05\","
          "\"\\uee06\","
          "\"\\uee07\","
          "\"\\uee08\","
          "\"\\uee09\","
          "\"\\uee0a\","
          "\"\\uee0b\","
          "\"\\uee0c\","
          "\"\\uee0d\","
          "\"\\uee0e\","
          "\"\\uee0f\","
          "\"\\n\","
          "\"\\r\""
          "]";
  body += "}";

  ScorerHttpDiagnosticScope diagnostic(
      generation, request_kind, prompt, body, options.ctx, options.threads,
      n_predict, max_output_chars, options.backend_device,
      options.flash_attention,
      WideToUtf8(options.llama_server_path),
      WideToUtf8(options.model_path),
      WideToUtf8(options.host) + ":" + std::to_string(options.port), value,
      debug);

  std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
  headers += L"Authorization: Bearer ";
  headers += Utf8ToWide(options.api_key);
  headers += L"\r\n";

  BOOL ok = WinHttpSendRequest(
      request,
      headers.c_str(),
      static_cast<DWORD>(-1L),
      body.data(),
      static_cast<DWORD>(body.size()),
      static_cast<DWORD>(body.size()),
      0);

  if (!ok) {
    const DWORD err = GetLastError();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    *debug = "winhttp_send_request_failed_" + std::to_string(err);
    return false;
  }

  ok = WinHttpReceiveResponse(request, nullptr);
  if (!ok) {
    const DWORD err = GetLastError();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    *debug = "winhttp_receive_response_failed_" + std::to_string(err);
    return false;
  }

  std::string response_body;

  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      const DWORD err = GetLastError();
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      *debug = "winhttp_query_data_available_failed_" + std::to_string(err);
      return false;
    }

    if (available == 0) {
      break;
    }

    std::string buffer(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), available, &read)) {
      const DWORD err = GetLastError();
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      *debug = "winhttp_read_data_failed_" + std::to_string(err);
      return false;
    }

    buffer.resize(read);

    if (response_body.size() + buffer.size() > kMaxHttpResponseBytes) {
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      *debug = "http_response_too_large";
      return false;
    }

    response_body += buffer;
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

  diagnostic.SetRawHttpResponseBody(response_body);

  std::string content;
  if (!mozc::zenz_scorer::ExtractJsonStringField(response_body, "content",
                                                  &content)) {
    *debug = "content_field_not_found";
    return false;
  }

  diagnostic.SetRawModelOutput(content);
  content = CleanGeneratedText(content, max_output_chars);
  diagnostic.SetCleanGeneratedText(content);
  if (content.empty()) {
    *debug = "empty_content";
    return false;
  }

  *value = std::move(content);
  *debug = "ok";
  diagnostic.MarkSuccess();
  return true;
}

void StartLlamaReadyProbeInBackground(const Options& options);

void StartLlamaServerInBackground(const Options& options) {
  bool expected = false;
  if (!g_llama_launch_started.compare_exchange_strong(expected, true)) {
    StartLlamaReadyProbeInBackground(options);
    return;
  }

  std::wstring launch_error;
  if (!LaunchLlamaServer(options, &launch_error)) {
    Debug(L"background launch failed: " + launch_error);
    g_llama_launch_started = false;
    g_llama_ready_probe_started = false;
    g_llama_server_ready = false;
    return;
  }

  Debug(L"background launch requested");
  StartLlamaReadyProbeInBackground(options);
}

void StartLlamaReadyProbeInBackground(const Options& options) {
  bool expected = false;
  if (!g_llama_ready_probe_started.compare_exchange_strong(expected, true)) {
    return;
  }

  const uint64_t runtime_generation = g_llama_runtime_generation.load();
  std::thread([options, runtime_generation]() {
    Debug(L"ready probe started");

    // Model loading + warmup may take several seconds on cold start.
    // This probe is intentionally outside Mozc's request path.
    for (int i = 0; i < 120; ++i) {
      if (runtime_generation != g_llama_runtime_generation.load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

      std::string value;
      std::string local_debug;

      if (HttpPostCompletion(
              options,
              "\xEE\xB8\x82\xEE\xB8\x80テスト\xEE\xB8\x81",
              kLlamaReadyProbeTimeoutMsec,
              8,
              0,
              "ready_probe",
              &value,
              &local_debug)) {
        if (runtime_generation != g_llama_runtime_generation.load()) {
          return;
        }
        g_llama_server_ready = true;
        g_llama_ready_probe_started = false;
        Debug(L"ready probe succeeded");
        return;
      }

      if (runtime_generation != g_llama_runtime_generation.load()) {
        return;
      }
      if (LlamaServerExited()) {
        if (!options.backend_device.empty() &&
            options.backend_device != "none") {
          std::lock_guard<std::mutex> lock(g_backend_device_mutex);
          if (!g_requested_backend_device.has_value() ||
              *g_requested_backend_device != options.backend_device) {
            return;
          }
          g_effective_backend_device = "none";
          Debug(L"requested backend device unavailable; falling back to CPU");
          Options fallback_options = options;
          fallback_options.backend_device = "none";
          StopLlamaServer();
          StartLlamaServerInBackground(fallback_options);
        } else {
          StopLlamaServer();
        }
        return;
      }

      if (i % 10 == 0) {
        Debug(L"ready probe waiting");
      }
    }

    if (runtime_generation == g_llama_runtime_generation.load()) {
      Debug(L"ready probe timeout");
      StopLlamaServer();
    }
  }).detach();
}

bool EnsureLlamaServerReadyWithinTimeout(const Options& options,
                                         uint32_t timeout_msec,
                                         std::string* debug) {
  if (g_llama_server_ready.load()) {
    *debug = "server_ready";
    return true;
  }

  StartLlamaServerInBackground(options);
  StartLlamaReadyProbeInBackground(options);

  const uint32_t wait_budget_msec =
      std::max<uint32_t>(
          std::max<uint32_t>(timeout_msec, 50),
          kMinLlamaReadyWaitMsec);

  constexpr uint32_t kReadyWaitStepMsec = 25;

  const DWORD start = GetTickCount();

  while (GetTickCount() - start < wait_budget_msec) {
    if (g_llama_server_ready.load()) {
      const DWORD waited = GetTickCount() - start;
      *debug = "server_ready_after_wait";
      Debug(L"server ready wait succeeded waited_msec=" +
            std::to_wstring(waited));
      return true;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(kReadyWaitStepMsec));
  }

  *debug = "server_loading";
  Debug(L"server ready wait timeout budget_msec=" +
        std::to_wstring(wait_budget_msec));
  return false;
}

bool MakePipeSecurityAttributes(
    PSECURITY_DESCRIPTOR* security_descriptor,
    SECURITY_ATTRIBUTES* security_attributes) {
  if (security_descriptor == nullptr || security_attributes == nullptr) {
    return false;
  }

  *security_descriptor = nullptr;
  *security_attributes = {};

  if (!BuildCurrentUserOnlyPipeSecurityDescriptor(security_descriptor)) {
    return false;
  }

  security_attributes->nLength = sizeof(*security_attributes);
  security_attributes->lpSecurityDescriptor = *security_descriptor;
  security_attributes->bInheritHandle = FALSE;
  return true;
}

void SendResponse(
    HANDLE pipe,
    uint32_t generation,
    uint32_t status,
    uint32_t latency_msec,
    const std::string& value,
    const std::string& debug) {
  ZenzWireResponseHeader header = {};
  header.magic = kZenzWireMagic;
  header.version = kZenzWireVersion;
  header.kind = kZenzWireKindResponse;
  header.generation = generation;
  header.status = status;
  header.latency_msec = latency_msec;
  header.value_size = static_cast<uint32_t>(value.size());
  header.debug_size = static_cast<uint32_t>(debug.size());

  WriteAll(pipe, &header, sizeof(header));

  if (!value.empty()) {
    WriteAll(pipe, value.data(), static_cast<uint32_t>(value.size()));
  }

  if (!debug.empty()) {
    WriteAll(pipe, debug.data(), static_cast<uint32_t>(debug.size()));
  }
}

void HandleClient(HANDLE pipe, const Options& options) {
  const DWORD start = GetTickCount();

  ZenzWireRequestHeader request_header = {};
  if (!ReadAll(pipe, &request_header, sizeof(request_header))) {
    return;
  }

  if (request_header.magic != kZenzWireMagic ||
      request_header.version != kZenzWireVersion ||
      request_header.kind != kZenzWireKindRequest) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "bad_request_header");
    return;
  }

  if (request_header.prompt_size == 0) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "empty_prompt");
    return;
  }

  if (request_header.prompt_size > kMaxPromptBytes) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "prompt_too_large");
    return;
  }

  if (request_header.backend_device_size > kMaxBackendDeviceBytes) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "backend_device_too_large");
    return;
  }

  const uint32_t timeout_msec = std::clamp<uint32_t>(
      request_header.timeout_msec == 0
          ? kMaxRequestTimeoutMsec
          : request_header.timeout_msec,
      50,
      kMaxRequestTimeoutMsec);

  const uint32_t max_output_chars = std::clamp<uint32_t>(
      request_header.max_output_chars == 0
          ? kMaxOutputChars
          : request_header.max_output_chars,
      1,
      kMaxOutputChars);

  std::string prompt(request_header.prompt_size, '\0');
  if (!ReadAll(pipe, prompt.data(), request_header.prompt_size)) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "failed_to_read_prompt");
    return;
  }

  std::string backend_device(request_header.backend_device_size, '\0');
  if (!backend_device.empty() &&
      !ReadAll(pipe, backend_device.data(), request_header.backend_device_size)) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "failed_to_read_backend_device");
    return;
  }
  if (!IsValidBackendDeviceName(backend_device)) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "invalid_backend_device");
    return;
  }

  Options request_options = options;
  request_options.backend_device = SelectBackendDevice(backend_device);

  Debug(L"request gen=" + std::to_wstring(request_header.generation) +
        L" " + RedactedUtf8Bytes(L"prompt", prompt));

  std::string debug;
  if (!EnsureLlamaServerReadyWithinTimeout(
          request_options, timeout_msec, &debug)) {
    const DWORD latency = GetTickCount() - start;
    SendResponse(pipe, request_header.generation, kStatusTimeout, latency, "",
                 debug);
    return;
  }

  std::string value;
  if (!HttpPostCompletion(
          request_options,
          prompt,
          timeout_msec,
          max_output_chars,
          request_header.generation,
          "client_request",
          &value,
          &debug)) {
    const DWORD latency = GetTickCount() - start;
    SendResponse(pipe, request_header.generation, kStatusError, latency, "",
                 debug);
    return;
  }

  const DWORD latency = GetTickCount() - start;

  Debug(L"response gen=" + std::to_wstring(request_header.generation) +
        L" latency=" + std::to_wstring(latency) +
        L" " + RedactedUtf8Bytes(L"value", value));

  SendResponse(pipe, request_header.generation, kStatusOk, latency, value,
               debug);
}

int RunServer(const Options& options) {
  Debug(L"server start " +
        RedactedWideChars(L"pipe_name", options.pipe_name) +
        L" " +
        RedactedWideChars(L"llama_server_path", options.llama_server_path) +
        L" " +
        RedactedWideChars(L"model_path", options.model_path));

  if (!options.random_ok) {
    Debug(L"secure random initialization failed");
    return 1;
  }

  if (!IsValidFlashAttentionValue(options.flash_attention)) {
    Debug(L"invalid flash attention option");
    return 1;
  }

  Debug(L"http_port_mode=random");
  Debug(L"api_key_bytes=" + std::to_wstring(options.api_key.size()));
  Debug(L"n_predict=" + std::to_wstring(options.n_predict));
  Debug(L"flash_attention=" + Utf8ToWide(options.flash_attention));

  while (!g_shutdown_requested.load()) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = {};

    if (!MakePipeSecurityAttributes(&sd, &sa)) {
      Debug(L"MakePipeSecurityAttributes failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    HANDLE pipe = ::CreateNamedPipeW(
        options.pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        65536,
        65536,
        0,
        &sa);

    if (sd != nullptr) {
      ::LocalFree(sd);
      sd = nullptr;
    }

    if (pipe == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      Debug(L"CreateNamedPipeW failed error=" + std::to_wstring(err));
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
    if (!connected) {
      const DWORD err = ::GetLastError();
      if (err != ERROR_PIPE_CONNECTED) {
        Debug(L"ConnectNamedPipe failed error=" + std::to_wstring(err));
        ::CloseHandle(pipe);
        continue;
      }
    }

    HandleClient(pipe, options);

    ::FlushFileBuffers(pipe);
    ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);
  }

  StopLlamaServer();
  return 0;
}

#else
using ZenzSocketHandle = int;
const ZenzSocketHandle kInvalidZenzSocket = -1;
// void CloseZenzSocket(ZenzSocketHandle h) { if (h >= 0) ::close(h); }

bool ReadAll(ZenzSocketHandle handle, void* data, uint32_t size) {
  uint8_t* ptr = static_cast<uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    ssize_t r = ::recv(handle, ptr, remaining, 0);
    if (r <= 0) {
      if (errno == EINTR) continue;
      return false;
    }
    ptr += r;
    remaining -= r;
  }
  return true;
}

bool WriteAll(ZenzSocketHandle handle, const void* data, uint32_t size) {
#if defined(__APPLE__)
  constexpr int kSendFlags = 0;
#else
  constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
  const uint8_t* ptr = static_cast<const uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    ssize_t w = ::send(handle, ptr, remaining, kSendFlags);
    if (w <= 0) {
      if (errno == EINTR) continue;
      return false;
    }
    ptr += w;
    remaining -= w;
  }
  return true;
}

void ResetLlamaReadyState() {
  g_llama_launch_started = false;
  g_llama_ready_probe_started = false;
  g_llama_server_ready = false;
}

void StopLlamaServer() {
  g_llama_runtime_generation.fetch_add(1);
  std::lock_guard<std::mutex> lock(g_llama_process_mutex);
  if (g_llama_process > 0) {
    ::kill(g_llama_process, SIGTERM);
    for (int i = 0; i < 30; ++i) {
      int status = 0;
      pid_t ret = ::waitpid(g_llama_process, &status, WNOHANG);
      if (ret == g_llama_process || (ret < 0 && errno == ECHILD)) {
        g_llama_process = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (g_llama_process > 0) {
      ::kill(g_llama_process, SIGKILL);
      ::waitpid(g_llama_process, nullptr, 0);
      g_llama_process = -1;
    }
  }
  ResetLlamaReadyState();
}

std::string SelectBackendDevice(const std::string& backend_device) {
  std::lock_guard<std::mutex> lock(g_backend_device_mutex);
  if (!g_requested_backend_device.has_value()) {
    g_requested_backend_device = backend_device;
    g_effective_backend_device = backend_device;
  } else if (*g_requested_backend_device != backend_device) {
    g_requested_backend_device = backend_device;
    g_effective_backend_device = backend_device;
    Debug("backend device changed; restarting llama-server");
    StopLlamaServer();
  }
  return g_effective_backend_device;
}

bool LlamaServerExited() {
  std::lock_guard<std::mutex> lock(g_llama_process_mutex);
  if (g_llama_process <= 0) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(g_llama_process, &status, WNOHANG);
  if (result == g_llama_process || (result < 0 && errno == ECHILD)) {
    g_llama_process = -1;
    return true;
  }
  return false;
}

void HandleSignal(int) {
  // Async signal handlers may only perform signal-safe operations. The accept
  // loop observes this flag after EINTR and performs process shutdown, waits,
  // mutex operations, and cleanup on normal control flow.
  g_posix_shutdown_requested = 1;
}

bool LaunchLlamaServer(const Options& options, std::string* error) {
  if (!FileExists(options.llama_server_path)) {
    *error = "llama_server_not_found";
    return false;
  }
  if (!FileExists(options.model_path)) {
    *error = "model_not_found";
    return false;
  }

  // Reserve the exact loopback port until the child has been created.  The
  // llama.cpp server does not currently accept an inherited listening socket,
  // so the reservation is released immediately before exec and the resulting
  // child image is checked before it is trusted.
  LoopbackPortReservation port_reservation;
  if (!port_reservation.Reserve(options.port)) {
    *error = "loopback_port_reservation_failed";
    return false;
  }

  Debug("launch llama-server port=random api_key_bytes=" +
        std::to_string(options.api_key.size()));

  std::vector<std::string> args = {
      options.llama_server_path,
      "-m",
      options.model_path,
      "-c",
      std::to_string(options.ctx),
      "-t",
      std::to_string(options.threads),
      "--host",
      "127.0.0.1",
      "--port",
      std::to_string(options.port),
      "--api-key",
      options.api_key,
      "--flash-attn",
      options.flash_attention,
  };
  if (!options.backend_device.empty()) {
    args.push_back("--device");
    args.push_back(options.backend_device);
  }
  std::vector<char*> c_args;
  c_args.reserve(args.size() + 1);
  for (auto& arg : args) {
    c_args.push_back(arg.data());
  }
  c_args.push_back(nullptr);

#if defined(__APPLE__)
  // fork() is unsafe once the scorer has started background threads.  Build
  // the complete argv and descriptor actions in the parent, then let Darwin's
  // posix_spawn implementation create and exec the child without running our
  // C++ code in a post-fork process.
  posix_spawn_file_actions_t file_actions;
  int spawn_result = ::posix_spawn_file_actions_init(&file_actions);
  if (spawn_result != 0) {
    *error = "posix_spawn_file_actions_failed";
    return false;
  }

  const struct {
    int descriptor;
    int flags;
  } redirects[] = {
      {STDIN_FILENO, O_RDONLY},
      {STDOUT_FILENO, O_WRONLY},
      {STDERR_FILENO, O_WRONLY},
  };
  for (const auto& redirect : redirects) {
    spawn_result = ::posix_spawn_file_actions_addopen(
        &file_actions, redirect.descriptor, "/dev/null", redirect.flags, 0);
    if (spawn_result != 0) {
      break;
    }
  }

  pid_t pid = -1;
  if (spawn_result == 0) {
    spawn_result = ::posix_spawn(
        &pid, options.llama_server_path.c_str(), &file_actions, nullptr,
        c_args.data(), *_NSGetEnviron());
  }
  ::posix_spawn_file_actions_destroy(&file_actions);
  if (spawn_result != 0 || pid <= 0) {
    *error = "posix_spawn_failed";
    return false;
  }
#else
  pid_t pid = ::fork();
  if (pid < 0) {
    *error = "fork_failed";
    return false;
  }

  if (pid == 0) {
    port_reservation.Close();
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    int fd = ::open("/dev/null", O_RDWR);
    if (fd >= 0) {
      ::dup2(fd, STDIN_FILENO);
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      if (fd > 2) ::close(fd);
    }

    ::execv(options.llama_server_path.c_str(), c_args.data());
    ::_exit(127);
  }
#endif

  port_reservation.Close();
  if (!WaitForExpectedLlamaProcessImage(pid, options.llama_server_path)) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    *error = "llama_process_identity_invalid";
    return false;
  }

  std::lock_guard<std::mutex> lock(g_llama_process_mutex);
  g_llama_process = pid;
  return true;
}

bool HttpPostCompletion(
    const Options& options,
    const std::string& prompt,
    uint32_t timeout_msec,
    uint32_t max_output_chars,
    uint32_t generation,
    const std::string& request_kind,
    std::string* value,
    std::string* debug) {
  value->clear();
  debug->clear();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    *debug = "socket_failed";
    return false;
  }

  struct timeval tv;
  tv.tv_sec = timeout_msec / 1000;
  tv.tv_usec = (timeout_msec % 1000) * 1000;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(options.port);
  if (inet_pton(AF_INET, options.host.c_str(), &server_addr.sin_addr) <= 0) {
    ::close(sock);
    *debug = "inet_pton_failed";
    return false;
  }

  if (::connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    ::close(sock);
    *debug = "connect_failed";
    return false;
  }

  const int requested_n_predict =
      max_output_chars > 0 ? static_cast<int>(max_output_chars)
                          : options.n_predict;
  const int n_predict =
      std::max(4, std::min(options.n_predict, requested_n_predict));

  std::string body;
  body += "{";
  body += "\"prompt\":\"";
  body += JsonEscapeUtf8(prompt);
  body += "\",";
  body += "\"n_predict\":";
  body += std::to_string(n_predict);
  body += ",";
  body += "\"temperature\":0.0,";
  body += "\"top_k\":1,";
  body += "\"top_p\":1.0,";
  body += "\"stream\":false,";
  body += "\"cache_prompt\":true,";
  body += "\"stop\":["
          "\"\\uee00\","
          "\"\\uee01\","
          "\"\\uee02\","
          "\"\\uee03\","
          "\"\\uee04\","
          "\"\\uee05\","
          "\"\\uee06\","
          "\"\\uee07\","
          "\"\\uee08\","
          "\"\\uee09\","
          "\"\\uee0a\","
          "\"\\uee0b\","
          "\"\\uee0c\","
          "\"\\uee0d\","
          "\"\\uee0e\","
          "\"\\uee0f\","
          "\"\\n\","
          "\"\\r\""
          "]";
  body += "}";

  ScorerHttpDiagnosticScope diagnostic(
      generation, request_kind, prompt, body, options.ctx, options.threads,
      n_predict, max_output_chars, options.backend_device,
      options.flash_attention,
      options.llama_server_path,
      options.model_path, options.host + ":" + std::to_string(options.port),
      value, debug);

  std::string request = "POST /completion HTTP/1.1\r\n";
  request += "Host: " + options.host + ":" + std::to_string(options.port) + "\r\n";
  request += "Content-Type: application/json; charset=utf-8\r\n";
  request += "Authorization: Bearer " + options.api_key + "\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  const char* ptr = request.c_str();
  size_t remaining = request.size();
  while (remaining > 0) {
#if defined(__APPLE__)
    constexpr int kSendFlags = 0;
#else
    constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
    ssize_t written = ::send(sock, ptr, remaining, kSendFlags);
    if (written <= 0) {
      ::close(sock);
      *debug = "send_failed";
      return false;
    }
    ptr += written;
    remaining -= written;
  }

  std::string response_body;
  char buffer[4096];
  while (true) {
    ssize_t read_bytes = ::recv(sock, buffer, sizeof(buffer), 0);
    if (read_bytes < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ::close(sock);
        *debug = "timeout";
        return false;
      }
      ::close(sock);
      *debug = "recv_failed";
      return false;
    }
    if (read_bytes == 0) {
      break;
    }
    if (response_body.size() + read_bytes > kMaxHttpResponseBytes) {
      ::close(sock);
      *debug = "http_response_too_large";
      return false;
    }
    response_body.append(buffer, read_bytes);
  }
  ::close(sock);

  size_t header_end = response_body.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    diagnostic.SetRawHttpResponseBody(response_body);
    *debug = "invalid_http_response";
    return false;
  }

  const std::string response_body_json =
      response_body.substr(header_end + 4);
  diagnostic.SetRawHttpResponseBody(response_body_json);

  std::string content;
  if (!mozc::zenz_scorer::ExtractJsonStringField(
          response_body_json, "content", &content)) {
    *debug = "content_field_not_found";
    return false;
  }

  diagnostic.SetRawModelOutput(content);
  content = CleanGeneratedText(content, max_output_chars);
  diagnostic.SetCleanGeneratedText(content);
  if (content.empty()) {
    *debug = "empty_content";
    return false;
  }

  *value = std::move(content);
  *debug = "ok";
  diagnostic.MarkSuccess();
  return true;
}

void StartLlamaReadyProbeInBackground(const Options& options);

void StartLlamaServerInBackground(const Options& options) {
  bool expected = false;
  if (!g_llama_launch_started.compare_exchange_strong(expected, true)) {
    StartLlamaReadyProbeInBackground(options);
    return;
  }

  std::string launch_error;
  if (!LaunchLlamaServer(options, &launch_error)) {
    Debug("background launch failed: " + launch_error);
    g_llama_launch_started = false;
    g_llama_ready_probe_started = false;
    g_llama_server_ready = false;
    return;
  }
  Debug("background launch requested");
  StartLlamaReadyProbeInBackground(options);
}

void StartLlamaReadyProbeInBackground(const Options& options) {
  bool expected = false;
  if (!g_llama_ready_probe_started.compare_exchange_strong(expected, true)) {
    return;
  }
  const uint64_t runtime_generation = g_llama_runtime_generation.load();
  std::thread([options, runtime_generation]() {
    Debug("ready probe started");
    for (int i = 0; i < 120; ++i) {
      if (runtime_generation != g_llama_runtime_generation.load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      std::string value;
      std::string local_debug;
      if (HttpPostCompletion(
              options,
              "\xEE\xB8\x82\xEE\xB8\x80テスト\xEE\xB8\x81",
              kLlamaReadyProbeTimeoutMsec,
              8,
              0,
              "ready_probe",
              &value,
              &local_debug)) {
        if (runtime_generation != g_llama_runtime_generation.load()) {
          return;
        }
        g_llama_server_ready = true;
        g_llama_ready_probe_started = false;
        Debug("ready probe succeeded");
        return;
      }

      if (runtime_generation != g_llama_runtime_generation.load()) {
        return;
      }
      if (LlamaServerExited()) {
        if (!options.backend_device.empty() &&
            options.backend_device != "none") {
          std::lock_guard<std::mutex> lock(g_backend_device_mutex);
          if (!g_requested_backend_device.has_value() ||
              *g_requested_backend_device != options.backend_device) {
            return;
          }
          g_effective_backend_device = "none";
          Debug("requested backend device unavailable; falling back to CPU");
          Options fallback_options = options;
          fallback_options.backend_device = "none";
          StopLlamaServer();
          StartLlamaServerInBackground(fallback_options);
        } else {
          StopLlamaServer();
        }
        return;
      }
      if (i % 10 == 0) {
        Debug("ready probe waiting");
      }
    }
    if (runtime_generation == g_llama_runtime_generation.load()) {
      Debug("ready probe timeout");
      StopLlamaServer();
    }
  }).detach();
}

uint64_t GetTickCountMsec() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

bool EnsureLlamaServerReadyWithinTimeout(const Options& options,
                                         uint32_t timeout_msec,
                                         std::string* debug) {
  if (g_llama_server_ready.load()) {
    *debug = "server_ready";
    return true;
  }

  StartLlamaServerInBackground(options);
  StartLlamaReadyProbeInBackground(options);

  const uint32_t wait_budget_msec =
      std::max<uint32_t>(
          std::max<uint32_t>(timeout_msec, 50),
          kMinLlamaReadyWaitMsec);

  constexpr uint32_t kReadyWaitStepMsec = 25;
  const uint64_t start = GetTickCountMsec();

  while (GetTickCountMsec() - start < wait_budget_msec) {
    if (g_llama_server_ready.load()) {
      const uint64_t waited = GetTickCountMsec() - start;
      *debug = "server_ready_after_wait";
      Debug("server ready wait succeeded waited_msec=" + std::to_string(waited));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kReadyWaitStepMsec));
  }

  *debug = "server_loading";
  Debug("server ready wait timeout budget_msec=" + std::to_string(wait_budget_msec));
  return false;
}

void SendResponse(
    ZenzSocketHandle pipe,
    uint32_t generation,
    uint32_t status,
    uint32_t latency_msec,
    const std::string& value,
    const std::string& debug) {
  ZenzWireResponseHeader header = {};
  header.magic = kZenzWireMagic;
  header.version = kZenzWireVersion;
  header.kind = kZenzWireKindResponse;
  header.generation = generation;
  header.status = status;
  header.latency_msec = latency_msec;
  header.value_size = static_cast<uint32_t>(value.size());
  header.debug_size = static_cast<uint32_t>(debug.size());

  WriteAll(pipe, &header, sizeof(header));

  if (!value.empty()) {
    WriteAll(pipe, value.data(), static_cast<uint32_t>(value.size()));
  }

  if (!debug.empty()) {
    WriteAll(pipe, debug.data(), static_cast<uint32_t>(debug.size()));
  }
}

void HandleClient(ZenzSocketHandle pipe, const Options& options) {
  const uint64_t start = GetTickCountMsec();

  ZenzWireRequestHeader request_header = {};
  if (!ReadAll(pipe, &request_header, sizeof(request_header))) {
    return;
  }

  if (request_header.magic != kZenzWireMagic ||
      request_header.version != kZenzWireVersion ||
      request_header.kind != kZenzWireKindRequest) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "", "bad_request_header");
    return;
  }

  if (request_header.prompt_size == 0) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "", "empty_prompt");
    return;
  }

  if (request_header.prompt_size > kMaxPromptBytes) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "", "prompt_too_large");
    return;
  }

  if (request_header.backend_device_size > kMaxBackendDeviceBytes) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "backend_device_too_large");
    return;
  }

  const uint32_t timeout_msec = std::clamp<uint32_t>(
      request_header.timeout_msec == 0 ? kMaxRequestTimeoutMsec : request_header.timeout_msec,
      50, kMaxRequestTimeoutMsec);

  const uint32_t max_output_chars = std::clamp<uint32_t>(
      request_header.max_output_chars == 0 ? kMaxOutputChars : request_header.max_output_chars,
      1, kMaxOutputChars);

  std::string prompt(request_header.prompt_size, '\0');
  if (!ReadAll(pipe, prompt.data(), request_header.prompt_size)) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "", "failed_to_read_prompt");
    return;
  }

  std::string backend_device(request_header.backend_device_size, '\0');
  if (!backend_device.empty() &&
      !ReadAll(pipe, backend_device.data(), request_header.backend_device_size)) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "failed_to_read_backend_device");
    return;
  }
  if (!IsValidBackendDeviceName(backend_device)) {
    SendResponse(pipe, request_header.generation, kStatusError, 0, "",
                 "invalid_backend_device");
    return;
  }

  Options request_options = options;
  request_options.backend_device = SelectBackendDevice(backend_device);

  Debug("request gen=" + std::to_string(request_header.generation) +
        " " + RedactedUtf8Bytes("prompt", prompt));

  std::string debug;
  if (!EnsureLlamaServerReadyWithinTimeout(request_options, timeout_msec,
                                           &debug)) {
    const uint64_t latency = GetTickCountMsec() - start;
    SendResponse(pipe, request_header.generation, kStatusTimeout, latency, "", debug);
    return;
  }

  std::string value;
  if (!HttpPostCompletion(request_options, prompt, timeout_msec,
                          max_output_chars, request_header.generation,
                          "client_request", &value, &debug)) {
    const uint64_t latency = GetTickCountMsec() - start;
    SendResponse(pipe, request_header.generation, kStatusError, latency, "", debug);
    return;
  }

  const uint64_t latency = GetTickCountMsec() - start;
  Debug("response gen=" + std::to_string(request_header.generation) +
        " latency=" + std::to_string(latency) +
        " " + RedactedUtf8Bytes("value", value));

  SendResponse(pipe, request_header.generation, kStatusOk, latency, value, debug);
}

bool RemoveOwnedSocketIfPresent(const std::string& socket_path) {
  struct stat socket_info = {};
  if (::lstat(socket_path.c_str(), &socket_info) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISSOCK(socket_info.st_mode) ||
      socket_info.st_uid != ::geteuid()) {
    return false;
  }
  return ::unlink(socket_path.c_str()) == 0;
}

int CreatePrivateUnixSocket(const std::string& socket_path) {
  struct sockaddr_un addr = {};
  if (socket_path.empty() || socket_path.front() != '/' ||
      socket_path.size() >= sizeof(addr.sun_path) ||
      !RemoveOwnedSocketIfPresent(socket_path)) {
    return -1;
  }

  const int server_sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_sock < 0) {
    return -1;
  }
  if (::fcntl(server_sock, F_SETFD, FD_CLOEXEC) != 0) {
    ::close(server_sock);
    return -1;
  }

  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
  const socklen_t addr_size = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + socket_path.size() + 1);
#if defined(__APPLE__)
  addr.sun_len = static_cast<unsigned char>(addr_size);
#endif

  const mode_t old_umask = ::umask(0077);
  const int bind_result =
      ::bind(server_sock, reinterpret_cast<struct sockaddr*>(&addr), addr_size);
  ::umask(old_umask);
  if (bind_result != 0) {
    const int error = errno;
    ::close(server_sock);
    errno = error;
    return -1;
  }
  if (::chmod(socket_path.c_str(), 0600) != 0) {
    const int error = errno;
    ::close(server_sock);
    ::unlink(socket_path.c_str());
    errno = error;
    return -1;
  }

  return server_sock;
}

int OpenPrivateLockFile(const std::string& lock_path) {
  const int lock_fd =
      ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
             0600);
  if (lock_fd < 0) {
    return -1;
  }

  struct stat lock_info = {};
  if (::fstat(lock_fd, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
      lock_info.st_uid != ::geteuid() || ::fchmod(lock_fd, 0600) != 0) {
    ::close(lock_fd);
    return -1;
  }
  return lock_fd;
}

int RunServer(const Options& options) {
  Debug("server start " + RedactedWideChars("pipe_name", options.pipe_name) +
        " " + RedactedWideChars("llama_server_path", options.llama_server_path) +
        " " + RedactedWideChars("model_path", options.model_path));

  if (!options.random_ok) {
    Debug("secure random initialization failed");
    return 1;
  }

  if (!IsValidFlashAttentionValue(options.flash_attention)) {
    Debug("invalid flash attention option");
    return 1;
  }

  Debug("http_port_mode=random");
  Debug("api_key_bytes=" + std::to_string(options.api_key.size()));
  Debug("n_predict=" + std::to_string(options.n_predict));
  Debug("flash_attention=" + options.flash_attention);

  std::string socket_path = options.pipe_name.empty() ?
      (std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + kDefaultPipeNameSuffix) : options.pipe_name;

  const int server_sock = CreatePrivateUnixSocket(socket_path);
  if (server_sock < 0) {
    Debug("Failed to create private UNIX domain socket");
    return 1;
  }

  if (::listen(server_sock, 128) < 0) {
    Debug("Failed to listen on UNIX domain socket");
    ::close(server_sock);
    ::unlink(socket_path.c_str());
    return 1;
  }

  while (!g_shutdown_requested.load() && !g_posix_shutdown_requested) {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = ::accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0) {
      if (errno == EINTR) continue;
      Debug("accept failed error=" + std::to_string(errno));
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    HandleClient(client_sock, options);
    ::close(client_sock);
  }

  ::close(server_sock);
  ::unlink(socket_path.c_str());
  StopLlamaServer();
  return 0;
}
#endif
}  // namespace

#if defined(_WIN32)
int wmain() {
  ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  HANDLE single_instance_mutex =
      ::CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);

  if (single_instance_mutex != nullptr &&
      ::GetLastError() == ERROR_ALREADY_EXISTS) {
    Debug(L"another scorer instance already exists");
    ::CloseHandle(single_instance_mutex);
    return 0;
  }

  const Options options = LoadOptions();
  const int result = RunServer(options);

  StopLlamaServer();

  if (single_instance_mutex != nullptr) {
    ::ReleaseMutex(single_instance_mutex);
    ::CloseHandle(single_instance_mutex);
  }

  return result;
}
#else
int main() {
  // Darwin does not provide MSG_NOSIGNAL. Ignore SIGPIPE so a client that
  // disconnects mid-response cannot terminate the scorer process.
  ::signal(SIGPIPE, SIG_IGN);

  struct sigaction sa = {};
  sa.sa_handler = HandleSignal;
  sigfillset(&sa.sa_mask);
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGHUP, &sa, nullptr);

  std::string lock_path =
      std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
#if !defined(GOOGLE_JAPANESE_INPUT_BUILD)
      "/.mozkey-ibg_zenz_scorer.lock";
#else
      "/.mozc_zenz_scorer.lock";
#endif
  int lock_fd = OpenPrivateLockFile(lock_path);
  if (lock_fd < 0) {
    Debug("failed to open private scorer lock");
    return 1;
  }
  struct flock fl = {};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (::fcntl(lock_fd, F_SETLK, &fl) < 0) {
    Debug("another scorer instance already exists");
    ::close(lock_fd);
    return 0;
  }

  const Options options = LoadOptions();
  const int result = RunServer(options);

  StopLlamaServer();
  ::close(lock_fd);
  ::unlink(lock_path.c_str());

  return result;
}
#endif
