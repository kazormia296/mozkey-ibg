#include "session/zenz_named_pipe_client.h"

#if !defined(_WIN32)
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "protocol/config.pb.h"
#include "session/zenz_named_pipe_endpoint.h"
#include "testing/gunit.h"
#include "zenz/zenz_wire_protocol.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mozc {
namespace session {
namespace {

// Mozkey IbG named-pipe/socket transport speaks wire version 2: upstream's v1
// request header plus backend_device_size, with the device name following the
// prompt.  Keep in sync with zenz_named_pipe_client.cc and zenz_scorer/main.cc.
constexpr uint16_t kTestZenzWireVersion = 2;

#pragma pack(push, 1)
struct TestZenzWireRequestHeader {
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

#if !defined(_WIN32)
constexpr uint32_t kTestZenzWireMagic = 0x315A4E5A;
constexpr uint16_t kTestZenzWireKindResponse = 2;
constexpr uint32_t kTestMaxWirePayloadBytes = 64 * 1024;

#pragma pack(push, 1)
struct TestZenzWireResponseHeader {
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

ZenzLiveResponse ConvertWithResponsePayloadSizes(uint32_t value_size,
                                                 uint32_t debug_size) {
  char temp_dir[] = "/tmp/zenz_payload_test_XXXXXX";
  EXPECT_NE(mkdtemp(temp_dir), nullptr);
  const std::string socket_path = std::string(temp_dir) + "/runtime.sock";

  const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_GE(server_fd, 0);

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  const socklen_t addr_size = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + socket_path.size() + 1);
#if defined(__APPLE__)
  addr.sun_len = static_cast<unsigned char>(addr_size);
#endif
  EXPECT_EQ(bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr),
                 addr_size),
            0);
  EXPECT_EQ(chmod(socket_path.c_str(), 0600), 0);
  EXPECT_EQ(listen(server_fd, 1), 0);

  constexpr uint32_t kGeneration = 73;
  std::thread server_thread([server_fd, value_size, debug_size]() {
    const int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      return;
    }

    // Wait for the request before sending the response.  Closing the client
    // socket immediately after sending the header races with the client's
    // request write on macOS, where SIGPIPE is not suppressed by MSG_NOSIGNAL.
    uint8_t request_header[sizeof(TestZenzWireRequestHeader)] = {};
    size_t received = 0;
    while (received < sizeof(request_header)) {
      const ssize_t count = recv(client_fd, request_header + received,
                                 sizeof(request_header) - received, 0);
      if (count > 0) {
        received += static_cast<size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        close(client_fd);
        return;
      }
    }

    const TestZenzWireResponseHeader header = {
        .magic = kTestZenzWireMagic,
        .version = kTestZenzWireVersion,
        .kind = kTestZenzWireKindResponse,
        .generation = kGeneration,
        .status = 0,
        .latency_msec = 1,
        .value_size = value_size,
        .debug_size = debug_size,
    };
#if defined(__APPLE__)
    constexpr int kSendFlags = 0;
#else
    constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
    send(client_fd, &header, sizeof(header), kSendFlags);
    close(client_fd);
  });

  ZenzNamedPipeClient client;
  ZenzLiveRequest request;
  request.generation = kGeneration;
  request.pipe_name = socket_path;
  request.timeout_msec = 100;
  const ZenzLiveResponse response = client.Convert(request);

  server_thread.join();
  close(server_fd);
  unlink(socket_path.c_str());
  rmdir(temp_dir);
  return response;
}

// Binds a user-only Unix-domain socket that satisfies the client's endpoint
// ownership and permission checks.
int CreateListeningSocket(const std::string &socket_path) {
  const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_GE(server_fd, 0);

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  const socklen_t addr_size = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + socket_path.size() + 1);
#if defined(__APPLE__)
  addr.sun_len = static_cast<unsigned char>(addr_size);
#endif
  EXPECT_EQ(bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr),
                 addr_size),
            0);
  EXPECT_EQ(chmod(socket_path.c_str(), 0600), 0);
  EXPECT_EQ(listen(server_fd, 1), 0);
  return server_fd;
}
#endif  // !defined(_WIN32)

TEST(ZenzNamedPipeClientTest, DefaultPipeMatchesConfigContract) {
  constexpr char kExpectedPipeName[] =
      R"(\\.\pipe\mozkey-ibg_zenz_scorer)";

  config::Config config;
  EXPECT_STREQ(kDefaultZenzNamedPipeName, kExpectedPipeName);
  EXPECT_EQ(config.zenz_live_correction_pipe_name(), kExpectedPipeName);
}

TEST(ZenzNamedPipeClientTest, RejectsOversizedBackendDeviceBeforeConnecting) {
  ZenzNamedPipeClient client;
  ZenzLiveRequest request;
  request.backend_device = std::string(129, 'x');

  const ZenzLiveResponse response = client.Convert(request);

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.debug, "backend_device_too_large");
}

TEST(ZenzNamedPipeClientTest, UnixFallback) {
#if !defined(_WIN32)
  // Create a temporary directory to act as HOME
  char temp_dir[] = "/tmp/zenz_test_XXXXXX";
  EXPECT_TRUE(mkdtemp(temp_dir) != nullptr);

  const char* original_home = getenv("HOME");
  const bool had_original_home = original_home != nullptr;
  const std::string saved_home = original_home ? original_home : "";
  setenv("HOME", temp_dir, 1);

  std::string socket_path = std::string(temp_dir) +
#if !defined(GOOGLE_JAPANESE_INPUT_BUILD)
                            "/.mozkey-ibg_zenz_scorer_pipe";
#else
                            "/.mozc_zenz_scorer_pipe";
#endif

  // Create a dummy Unix domain socket
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  const socklen_t addr_size = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + socket_path.size() + 1);
#if defined(__APPLE__)
  addr.sun_len = static_cast<unsigned char>(addr_size);
#endif

  EXPECT_EQ(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), addr_size), 0);
  EXPECT_EQ(chmod(socket_path.c_str(), 0600), 0);
  EXPECT_EQ(listen(fd, 1), 0);

  std::thread server_thread([fd]() {
    int client_fd = accept(fd, nullptr, nullptr);
    if (client_fd >= 0) {
      close(client_fd);
    }
  });

  ZenzNamedPipeClient client;
  ZenzLiveRequest request;
  // Windows-style named pipe path
  request.pipe_name = kDefaultZenzNamedPipeName;
  request.timeout_msec = 100;

  // The OSS Linux client uses Mozkey's product-specific fallback socket.
  ZenzLiveResponse response = client.Convert(request);

  server_thread.join();

  // Since our dummy socket closes immediately, the client will fail at write or read,
  // but crucially it should NOT fail with "pipe_open_failed" or "invalid_pipe_name".
  EXPECT_NE(response.debug, "pipe_open_failed");
  EXPECT_NE(response.debug, "invalid_pipe_name");

  close(fd);
  unlink(socket_path.c_str());
  rmdir(temp_dir);
  if (had_original_home) {
    setenv("HOME", saved_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
#endif
}

TEST(ZenzNamedPipeClientTest, RejectsOversizedResponseBeforeAllocation) {
#if !defined(_WIN32)
  for (const auto &[value_size, debug_size] : {
           std::pair<uint32_t, uint32_t>{
               std::numeric_limits<uint32_t>::max(), 0},
           std::pair<uint32_t, uint32_t>{
               0, std::numeric_limits<uint32_t>::max()},
           std::pair<uint32_t, uint32_t>{
               kTestMaxWirePayloadBytes, 1},
       }) {
    SCOPED_TRACE(testing::Message()
                 << "value_size=" << value_size
                 << " debug_size=" << debug_size);
    const ZenzLiveResponse response =
        ConvertWithResponsePayloadSizes(value_size, debug_size);
    EXPECT_FALSE(response.ok);
    EXPECT_EQ(response.debug, "pipe_response_payload_too_large");
  }
#endif
}

#if defined(_WIN32)

bool AllowSyntheticPipePeer(void* handle) {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

using ::mozc::zenz::kZenzWireKindRequest;
using ::mozc::zenz::kZenzWireKindResponse;
using ::mozc::zenz::kZenzWireMagic;
using ::mozc::zenz::kZenzWireStatusOk;
using ::mozc::zenz::ZenzWireResponseHeader;

constexpr uint16_t kZenzWireVersion = kTestZenzWireVersion;
using ZenzWireRequestHeader = TestZenzWireRequestHeader;

struct TestPipeName {
  std::string utf8;
  std::wstring wide;
};

TestPipeName MakeTestPipeName() {
  static std::atomic<uint32_t> sequence{0};
  const std::string suffix =
      "mozc_zenz_named_pipe_client_test_" +
      std::to_string(::GetCurrentProcessId()) + "_" +
      std::to_string(::GetTickCount64()) + "_" +
      std::to_string(sequence.fetch_add(1));
  TestPipeName result;
  result.utf8 = "\\\\.\\pipe\\" + suffix;
  result.wide = L"\\\\.\\pipe\\" + std::wstring(suffix.begin(), suffix.end());
  return result;
}

HANDLE CreateServerPipe(const std::wstring& name) {
  return ::CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
      nullptr);
}

bool ConnectServer(HANDLE pipe) {
  if (::ConnectNamedPipe(pipe, nullptr)) {
    return true;
  }
  return ::GetLastError() == ERROR_PIPE_CONNECTED;
}

bool ReadExact(HANDLE pipe, void* data, uint32_t size) {
  uint8_t* current = static_cast<uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    DWORD read = 0;
    if (!::ReadFile(pipe, current, remaining, &read, nullptr) || read == 0) {
      return false;
    }
    current += read;
    remaining -= read;
  }
  return true;
}

bool WriteExact(HANDLE pipe, const void* data, uint32_t size) {
  const uint8_t* current = static_cast<const uint8_t*>(data);
  uint32_t remaining = size;
  while (remaining > 0) {
    DWORD written = 0;
    if (!::WriteFile(pipe, current, remaining, &written, nullptr) ||
        written == 0) {
      return false;
    }
    current += written;
    remaining -= written;
  }
  return true;
}

bool ReadRequest(HANDLE pipe, ZenzWireRequestHeader* header,
                 std::string* prompt,
                 std::string* backend_device = nullptr) {
  if (!ReadExact(pipe, header, sizeof(*header))) {
    return false;
  }
  if (header->prompt_size > 8192 || header->backend_device_size > 128) {
    return false;
  }
  prompt->assign(header->prompt_size, '\0');
  if (!prompt->empty() &&
      !ReadExact(pipe, prompt->data(), header->prompt_size)) {
    return false;
  }
  std::string device(header->backend_device_size, '\0');
  if (!device.empty() &&
      !ReadExact(pipe, device.data(), header->backend_device_size)) {
    return false;
  }
  if (backend_device != nullptr) {
    *backend_device = std::move(device);
  }
  return true;
}

ZenzLiveRequest MakeRequest(const TestPipeName& name, uint32_t generation) {
  ZenzLiveRequest request;
  request.generation = generation;
  request.key = "てすと";
  request.prompt = "prompt-data";
  request.pipe_name = name.utf8;
  request.timeout_msec = 180;
  request.max_output_chars = 48;
  return request;
}

TEST(ZenzNamedPipeClientTest, StopInterruptsMissingResponseAndJoinsWorker) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE server = CreateServerPipe(name.wide);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);
  std::atomic<bool> received{false};
  std::atomic<bool> done{false};
  std::thread server_thread([&] {
    ZenzWireRequestHeader request = {};
    std::string prompt;
    if (ConnectServer(server) && ReadRequest(server, &request, &prompt)) {
      received = true;
      for (int i = 0; i < 5000 && !done.load(); ++i) {
        ::Sleep(1);
      }
    }
    ::CloseHandle(server);
  });
  auto client = std::make_unique<ZenzNamedPipeClient>(
      ZenzNamedPipeClient::kDefaultTransportSafetyTimeoutMsec,
      &AllowSyntheticPipePeer);
  auto* client_ptr = client.get();
  ZenzLiveCorrector corrector(std::move(client));
  corrector.Submit(MakeRequest(name, 101));
  for (int i = 0; i < 2000 && !received.load(); ++i) {
    ::Sleep(1);
  }
  const auto started = std::chrono::steady_clock::now();
  corrector.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  done = true;
  server_thread.join();
  EXPECT_TRUE(received.load());
  EXPECT_LT(elapsed, std::chrono::seconds(2));

  EXPECT_FALSE(corrector.TakeResult(101).has_value());
  EXPECT_TRUE(client_ptr->IsStopRequested());
  corrector.Start();
  EXPECT_FALSE(client_ptr->IsStopRequested());
  corrector.Stop();
}

TEST(ZenzNamedPipeClientTest, PreCancelledRequestDoesNotLaunchOrConnect) {
  ZenzNamedPipeClient client;
  client.RequestStop();
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.Convert(MakeRequest(MakeTestPipeName(), 102)).ok);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));
}

TEST(ZenzNamedPipeClientTest, ImmediateFullResponseSucceeds) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE server = CreateServerPipe(name.wide);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);

  std::atomic<bool> client_done{false};
  std::atomic<bool> server_ok{false};
  std::thread server_thread([&] {
    ZenzWireRequestHeader request = {};
    std::string prompt;
    std::string backend_device;
    if (!ConnectServer(server) ||
        !ReadRequest(server, &request, &prompt, &backend_device)) {
      ::CloseHandle(server);
      return;
    }

    const std::string value = "converted";
    const std::string debug = "test_backend";
    ZenzWireResponseHeader response = {};
    response.magic = kZenzWireMagic;
    response.version = kZenzWireVersion;
    response.kind = kZenzWireKindResponse;
    response.generation = request.generation;
    response.status = kZenzWireStatusOk;
    response.latency_msec = 12;
    response.value_size = static_cast<uint32_t>(value.size());
    response.debug_size = static_cast<uint32_t>(debug.size());

    server_ok =
        request.magic == kZenzWireMagic &&
        request.version == kZenzWireVersion &&
        request.kind == kZenzWireKindRequest &&
        prompt == "prompt-data" &&
        backend_device == "Vulkan0" &&
        WriteExact(server, &response, sizeof(response)) &&
        WriteExact(server, value.data(), response.value_size) &&
        WriteExact(server, debug.data(), response.debug_size);

    for (int i = 0; server_ok.load() && !client_done.load() && i < 2000; ++i) {
      ::Sleep(1);
    }
    ::CloseHandle(server);
  });

  ZenzNamedPipeClient client(1000, &AllowSyntheticPipePeer);
  ZenzLiveRequest live_request = MakeRequest(name, 73);
  live_request.backend_device = "Vulkan0";
  const ZenzLiveResponse response = client.Convert(live_request);
  client_done = true;
  server_thread.join();

  EXPECT_TRUE(server_ok.load());
  EXPECT_TRUE(response.ok);
  EXPECT_FALSE(response.timeout);
  EXPECT_EQ(response.generation, 73);
  EXPECT_EQ(response.value, "converted");
  EXPECT_EQ(response.debug, "test_backend");
}

TEST(ZenzNamedPipeClientTest, MissingResponseHeaderTimesOut) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE server = CreateServerPipe(name.wide);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);

  std::atomic<bool> server_received{false};
  std::thread server_thread([&] {
    ZenzWireRequestHeader request = {};
    std::string prompt;
    if (ConnectServer(server) && ReadRequest(server, &request, &prompt)) {
      server_received = true;
      ::Sleep(500);
    }
    ::CloseHandle(server);
  });

  ZenzNamedPipeClient client(200, &AllowSyntheticPipePeer);
  const auto started = std::chrono::steady_clock::now();
  const ZenzLiveResponse response = client.Convert(MakeRequest(name, 74));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  server_thread.join();

  EXPECT_TRUE(server_received.load());
  EXPECT_FALSE(response.ok);
  EXPECT_TRUE(response.timeout);
  EXPECT_EQ(response.debug, "pipe_read_header_timeout");
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            2000);
}

TEST(ZenzNamedPipeClientTest, PartialResponsePayloadTimesOut) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE server = CreateServerPipe(name.wide);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);

  std::atomic<bool> header_written{false};
  std::thread server_thread([&] {
    ZenzWireRequestHeader request = {};
    std::string prompt;
    if (!ConnectServer(server) || !ReadRequest(server, &request, &prompt)) {
      ::CloseHandle(server);
      return;
    }

    ZenzWireResponseHeader response = {};
    response.magic = kZenzWireMagic;
    response.version = kZenzWireVersion;
    response.kind = kZenzWireKindResponse;
    response.generation = request.generation;
    response.status = kZenzWireStatusOk;
    response.value_size = 8;
    header_written = WriteExact(server, &response, sizeof(response));
    ::Sleep(500);
    ::CloseHandle(server);
  });

  ZenzNamedPipeClient client(200, &AllowSyntheticPipePeer);
  const ZenzLiveResponse response = client.Convert(MakeRequest(name, 75));
  server_thread.join();

  EXPECT_TRUE(header_written.load());
  EXPECT_FALSE(response.ok);
  EXPECT_TRUE(response.timeout);
  EXPECT_EQ(response.debug, "pipe_read_payload_timeout");
}

TEST(ZenzNamedPipeClientTest, OversizedResponsePayloadIsRejected) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE server = CreateServerPipe(name.wide);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);

  std::atomic<bool> header_written{false};
  std::thread server_thread([&] {
    ZenzWireRequestHeader request = {};
    std::string prompt;
    if (!ConnectServer(server) || !ReadRequest(server, &request, &prompt)) {
      ::CloseHandle(server);
      return;
    }

    ZenzWireResponseHeader response = {};
    response.magic = kZenzWireMagic;
    response.version = kZenzWireVersion;
    response.kind = kZenzWireKindResponse;
    response.generation = request.generation;
    response.status = kZenzWireStatusOk;
    response.value_size = 1024 * 1024 + 1;
    header_written = WriteExact(server, &response, sizeof(response));
    ::Sleep(50);
    ::CloseHandle(server);
  });

  ZenzNamedPipeClient client(500, &AllowSyntheticPipePeer);
  const ZenzLiveResponse response = client.Convert(MakeRequest(name, 76));
  server_thread.join();

  EXPECT_TRUE(header_written.load());
  EXPECT_FALSE(response.ok);
  EXPECT_FALSE(response.timeout);
  EXPECT_EQ(response.debug, "pipe_response_payload_too_large");
}

TEST(ZenzNamedPipeClientTest, SpoofedPipeIsRejectedBeforeAnyPromptBytes) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE server = CreateServerPipe(name.wide);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);

  std::atomic<bool> received_prompt{false};
  std::thread server_thread([&] {
    if (ConnectServer(server)) {
      char byte = 0;
      DWORD bytes_read = 0;
      if (::ReadFile(server, &byte, 1, &bytes_read, nullptr) &&
          bytes_read > 0) {
        received_prompt = true;
      }
    }
    ::CloseHandle(server);
  });

  // This fake server runs in the test process, not mozc_zenz_scorer.exe.
  // The real verifier must reject it before WriteFile is called.
  ZenzNamedPipeClient client(500);
  const ZenzLiveResponse response = client.Convert(MakeRequest(name, 78));
  server_thread.join();

  // The client may close immediately after rejecting the peer. In that
  // case ConnectNamedPipe can return ERROR_NO_DATA even though CreateFileW
  // successfully connected; the peer rejection result is the invariant.
  EXPECT_FALSE(received_prompt.load());
  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.debug, "pipe_peer_untrusted");
}

TEST(ZenzNamedPipeClientTest, FirstPipeInstanceRejectsExistingPipeName) {
  const TestPipeName name = MakeTestPipeName();
  HANDLE first = CreateServerPipe(name.wide);
  ASSERT_NE(first, INVALID_HANDLE_VALUE);

  HANDLE second = ::CreateNamedPipeW(
      name.wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, nullptr);
  EXPECT_EQ(second, INVALID_HANDLE_VALUE);
  if (second != INVALID_HANDLE_VALUE) {
    ::CloseHandle(second);
  }
  ::CloseHandle(first);
}

// Resolve a separately built test helper, not a server running in this test
// process. Bazel may place data dependencies in runfiles instead of the binary
// output directory, so check both locations.
std::wstring ExternalFakeServerHelperPath() {
  const wchar_t helper_name[] =
      L"zenz_named_pipe_fake_server_test_helper.exe";
  wchar_t module_path[32768] = {};
  const DWORD len = ::GetModuleFileNameW(nullptr, module_path, 32768);
  if (len > 0 && len < 32768) {
    const std::wstring current(module_path, len);
    const size_t separator = current.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
      const std::wstring sibling =
          current.substr(0, separator + 1) + helper_name;
      if (::GetFileAttributesW(sibling.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return sibling;
      }
    }
  }

  wchar_t runfiles[32768] = {};
  wchar_t workspace[1024] = {};
  const DWORD runfiles_len =
      ::GetEnvironmentVariableW(L"TEST_SRCDIR", runfiles, 32768);
  const DWORD workspace_len =
      ::GetEnvironmentVariableW(L"TEST_WORKSPACE", workspace, 1024);
  if (runfiles_len == 0 || runfiles_len >= 32768 ||
      workspace_len == 0 || workspace_len >= 1024) {
    return L"";
  }
  const std::wstring path = std::wstring(runfiles, runfiles_len) + L"\\" +
                            std::wstring(workspace, workspace_len) +
                            L"\\session\\" + helper_name;
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
             ? path
             : L"";
}

TEST(ZenzNamedPipeClientTest, ExternalProcessSpoofRejectedBeforeAnyBytes) {
  const TestPipeName name = MakeTestPipeName();
  const std::wstring helper_path = ExternalFakeServerHelperPath();
  ASSERT_FALSE(helper_path.empty()) << "Separate fake pipe helper not found";

  std::wstring command_line =
      L"\"" + helper_path + L"\" \"" + name.wide + L"\"";
  std::vector<wchar_t> command_chars(command_line.begin(),
                                    command_line.end());
  command_chars.push_back(L'\0');
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  ASSERT_NE(::CreateProcessW(helper_path.c_str(), command_chars.data(),
                             nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &startup, &process), FALSE)
      << "Cannot start external fake server: " << ::GetLastError();
  ::CloseHandle(process.hThread);

  // No launch of the real scorer is needed: a random, test-specific pipe name
  // is provided explicitly. Wait for the fake process to claim the name.
  bool ready = false;
  for (int i = 0; i < 500; ++i) {
    if (::WaitNamedPipeW(name.wide.c_str(), 0)) {
      ready = true;
      break;
    }
    if (::WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
      break;
    }
    ::Sleep(10);
  }

  ZenzLiveResponse response;
  if (ready) {
    ZenzNamedPipeClient client(1000);  // Genuine production verifier.
    response = client.Convert(MakeRequest(name, 79));
  }

  const DWORD wait = ::WaitForSingleObject(process.hProcess, 5000);
  if (wait != WAIT_OBJECT_0) {
    ::TerminateProcess(process.hProcess, 99);
    ::WaitForSingleObject(process.hProcess, 5000);
  }
  DWORD exit_code = 0;
  const bool exit_code_ok =
      ::GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
  ::CloseHandle(process.hProcess);

  ASSERT_TRUE(ready) << "External helper did not claim the test pipe";
  EXPECT_EQ(wait, WAIT_OBJECT_0) << "External helper did not finish";
  ASSERT_TRUE(exit_code_ok);
  EXPECT_EQ(exit_code, 0u) << "Fake server observed bytes or failed";
  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.debug, "pipe_peer_untrusted");
}

TEST(ZenzNamedPipeClientTest, OversizedPromptIsRejectedBeforePipeOpen) {
  ZenzLiveRequest request = MakeRequest(MakeTestPipeName(), 77);
  request.prompt.assign(8193, 'x');

  ZenzNamedPipeClient client(500);
  const ZenzLiveResponse response = client.Convert(request);

  EXPECT_FALSE(response.ok);
  EXPECT_FALSE(response.timeout);
  EXPECT_EQ(response.debug, "prompt_too_large");
}

#endif  // _WIN32

#if !defined(_WIN32)

// The POSIX transport honours the same transport safety timeout and Stop()
// interruption contract as the hardened Windows named-pipe transport.
TEST(ZenzNamedPipeClientTest, PosixMissingResponseHeaderTimesOut) {
  char temp_dir[] = "/tmp/zenz_timeout_test_XXXXXX";
  ASSERT_NE(mkdtemp(temp_dir), nullptr);
  const std::string socket_path = std::string(temp_dir) + "/runtime.sock";
  const int server_fd = CreateListeningSocket(socket_path);

  std::atomic<bool> client_done{false};
  std::thread server_thread([&] {
    const int client_fd = accept(server_fd, nullptr, nullptr);
    for (int i = 0; i < 5000 && !client_done.load(); ++i) {
      usleep(1000);
    }
    if (client_fd >= 0) {
      close(client_fd);
    }
  });

  ZenzNamedPipeClient client(200);
  ZenzLiveRequest request;
  request.generation = 74;
  request.prompt = "prompt-data";
  request.pipe_name = socket_path;
  request.timeout_msec = 100;
  const auto started = std::chrono::steady_clock::now();
  const ZenzLiveResponse response = client.Convert(request);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  client_done = true;
  server_thread.join();
  close(server_fd);
  unlink(socket_path.c_str());
  rmdir(temp_dir);

  EXPECT_FALSE(response.ok);
  EXPECT_TRUE(response.timeout);
  EXPECT_EQ(response.debug, "pipe_read_header_timeout");
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(ZenzNamedPipeClientTest, PosixPreCancelledRequestDoesNotConnect) {
  char temp_dir[] = "/tmp/zenz_cancel_test_XXXXXX";
  ASSERT_NE(mkdtemp(temp_dir), nullptr);
  const std::string socket_path = std::string(temp_dir) + "/runtime.sock";
  const int server_fd = CreateListeningSocket(socket_path);

  ZenzNamedPipeClient client;
  client.RequestStop();
  ZenzLiveRequest request;
  request.generation = 102;
  request.prompt = "prompt-data";
  request.pipe_name = socket_path;
  request.timeout_msec = 100;
  const auto started = std::chrono::steady_clock::now();
  const ZenzLiveResponse response = client.Convert(request);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));
  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.debug, "pipe_open_failed");

  close(server_fd);
  unlink(socket_path.c_str());
  rmdir(temp_dir);
}

TEST(ZenzNamedPipeClientTest, PosixOversizedPromptIsRejectedBeforeSocketOpen) {
  ZenzLiveRequest request;
  request.generation = 77;
  request.pipe_name = "/nonexistent/mozkey-ibg-zenz-test.sock";
  request.prompt.assign(8193, 'x');

  ZenzNamedPipeClient client(500);
  const ZenzLiveResponse response = client.Convert(request);

  EXPECT_FALSE(response.ok);
  EXPECT_FALSE(response.timeout);
  EXPECT_EQ(response.debug, "prompt_too_large");
}

#endif  // !defined(_WIN32)

}  // namespace
}  // namespace session
}  // namespace mozc
