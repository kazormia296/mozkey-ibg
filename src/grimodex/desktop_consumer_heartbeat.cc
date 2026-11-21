// Copyright 2026 The Mozkey Authors

#include "grimodex/desktop_consumer_heartbeat.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "base/environ.h"
#include "base/thread.h"
#include "base/version.h"
#include "grimodex/consumer_file_registrar.h"
#include "grimodex/consumer_handshake.h"
#include "grimodex/consumer_heartbeat.h"

#if defined(_WIN32)
#include <windows.h>

#include <cstdint>

#include "base/file_util.h"
#include "base/system_util.h"
#include "base/win32/wide_char.h"
#include "grimodex/consumer_file_registrar_windows.h"
#include "grimodex/consumer_runtime_capability.h"
#include "grimodex/protocol_v1_windows_secure_reader.h"
#elif defined(__APPLE__)
#include <sys/stat.h>

#include "base/file_util.h"
#include "base/system_util.h"
#include "grimodex/consumer_file_registrar_posix.h"
#include "grimodex/consumer_runtime_capability.h"
#include "grimodex/protocol_v1_secure_reader.h"
#endif

namespace mozc::grimodex {
namespace {

#if defined(MOZC_BUILD)
constexpr bool kIsMozkeyBuild = true;
#else
constexpr bool kIsMozkeyBuild = false;
#endif

#if defined(_WIN32) || defined(__APPLE__)
constexpr bool kIsSupportedDesktopPlatform = true;
#else
constexpr bool kIsSupportedDesktopPlatform = false;
#endif

constexpr bool kDesktopConsumerHeartbeatEnabled =
    desktop_consumer_heartbeat_internal::ShouldEnable(
        kIsMozkeyBuild, kIsSupportedDesktopPlatform);
#if defined(MOZC_BUILD) && (defined(_WIN32) || defined(__APPLE__))
static_assert(kDesktopConsumerHeartbeatEnabled);
#else
static_assert(!kDesktopConsumerHeartbeatEnabled);
#endif

#if defined(_WIN32) || defined(__APPLE__)

struct PlatformRegistration {
  std::unique_ptr<ConsumerFileRegistrar> registrar;
  ConsumerHandshake metadata;
  std::function<bool()> zenz_available;
};

#if defined(_WIN32)

bool IsNonemptyRegularRuntimeFile(absl::string_view path) {
  const std::wstring wide_path = win32::Utf8ToWide(path);
  if (wide_path.empty()) {
    return false;
  }
  WIN32_FILE_ATTRIBUTE_DATA info = {};
  if (!::GetFileAttributesExW(wide_path.c_str(), GetFileExInfoStandard,
                              &info)) {
    return false;
  }
  if ((info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return false;
  }
  const uint64_t size =
      (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
      static_cast<uint64_t>(info.nFileSizeLow);
  return size > 0;
}

bool WindowsZenzRuntimeAvailable() {
  const std::string server_directory = SystemUtil::GetServerDirectory();
  if (server_directory.empty()) {
    return false;
  }
  return HasCompleteWindowsZenzRuntime(
      [&server_directory](absl::string_view relative_path) {
        return IsNonemptyRegularRuntimeFile(
            FileUtil::JoinPath(server_directory, relative_path));
      });
}

absl::StatusOr<PlatformRegistration> MakePlatformRegistration(
    absl::string_view override_root) {
  const absl::StatusOr<std::string> root =
      ResolveWindowsProtocolV1Root(override_root);
  if (!root.ok()) {
    return root.status();
  }
  return PlatformRegistration{
      .registrar = std::make_unique<WindowsConsumerFileRegistrar>(*root),
      .metadata =
          ConsumerHandshake{
              .consumer_id = std::string(kTsfConsumerId),
              .name = "Mozkey IbG for Grimodex on Windows",
              .version = Version::GetMozkeyReleaseVersion(),
              .platform = "windows",
              .last_seen = "",
              .capabilities =
                  ConsumerCapabilities{
                      .profile = true,
                      .dynamic_dictionary = true,
                      .zenzai_v3_conditions = false,
                      .application_scoping = true,
                  },
          },
      .zenz_available = WindowsZenzRuntimeAvailable,
  };
}

#elif defined(__APPLE__)

bool IsPackagedMacosRuntimeFile(absl::string_view path) {
  struct stat info = {};
  if (::lstat(std::string(path).c_str(), &info) != 0 ||
      !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return false;
  }
  const std::string filename = FileUtil::Basename(path);
  if ((filename == "mozc_zenz_scorer" || filename == "mozc_zenz_guard" ||
       filename == "llama-server") &&
      (info.st_mode & S_IXUSR) == 0) {
    return false;
  }
  return true;
}

bool MacosZenzRuntimeAvailable() {
  // Same location session/zenz_client_factory.cc launches the scorer from:
  // MozkeyIbG.app/Contents/Resources/ZenzRuntime.
  const std::string server_directory = SystemUtil::GetServerDirectory();
  if (server_directory.empty()) {
    return false;
  }
  const std::string runtime =
      FileUtil::JoinPath(server_directory, "ZenzRuntime");
  return HasCompleteMacosZenzRuntime(
      [&runtime](absl::string_view relative_path) {
        return IsPackagedMacosRuntimeFile(
            FileUtil::JoinPath(runtime, relative_path));
      });
}

absl::StatusOr<PlatformRegistration> MakePlatformRegistration(
    absl::string_view override_root) {
  const std::string home = Environ::GetEnv("HOME");
  if (override_root.empty() && home.empty()) {
    return absl::FailedPreconditionError(
        "HOME is unavailable for the Grimodex consumer root");
  }
  const std::string root = ResolveProtocolV1Root(
      override_root, Environ::GetEnv("XDG_DATA_HOME"), home);
  return PlatformRegistration{
      .registrar = std::make_unique<PosixConsumerFileRegistrar>(root),
      .metadata =
          ConsumerHandshake{
              .consumer_id = std::string(kImkitConsumerId),
              .name = "Mozkey IbG for Grimodex on macOS",
              .version = Version::GetMozkeyReleaseVersion(),
              .platform = "macos",
              .last_seen = "",
              .capabilities =
                  ConsumerCapabilities{
                      .profile = true,
                      .dynamic_dictionary = true,
                      .zenzai_v3_conditions = false,
                      .application_scoping = true,
                  },
          },
      .zenz_available = MacosZenzRuntimeAvailable,
  };
}

#endif  // defined(_WIN32)

class DesktopConsumerHeartbeatImpl final
    : public DesktopConsumerHeartbeat {
 public:
  explicit DesktopConsumerHeartbeatImpl(PlatformRegistration registration)
      : registrar_(std::move(registration.registrar)),
        heartbeat_(*registrar_, std::move(registration.metadata),
                   [] { return absl::Now(); }),
        zenz_available_(std::move(registration.zenz_available)) {
    RefreshAndLog();
    thread_ = Thread([this] { ThreadMain(); });
  }

  ~DesktopConsumerHeartbeatImpl() override {
    stop_.Notify();
    if (thread_.Joinable()) {
      thread_.Join();
    }
  }

 private:
  void RefreshAndLog() {
    const bool zenz_available = zenz_available_();
    heartbeat_.SetZenzaiV3ConditionsAvailable(zenz_available);
    absl::Status status = heartbeat_.RefreshNow();
    // If package removal raced the publication, immediately replace a stale
    // true capability with the conservative false value.
    if (status.ok() && zenz_available && !zenz_available_()) {
      heartbeat_.SetZenzaiV3ConditionsAvailable(false);
      status = heartbeat_.RefreshNow();
    }
    if (!status.ok()) {
      LOG(ERROR) << "Failed to refresh Grimodex desktop consumer: "
                 << status;
    }
  }

  void ThreadMain() {
    while (!stop_.WaitForNotificationWithTimeout(kConsumerRefreshInterval)) {
      RefreshAndLog();
    }
  }

  std::unique_ptr<ConsumerFileRegistrar> registrar_;
  ConsumerHeartbeat heartbeat_;
  std::function<bool()> zenz_available_;
  absl::Notification stop_;
  Thread thread_;
};

#endif  // defined(_WIN32) || defined(__APPLE__)

}  // namespace

std::unique_ptr<DesktopConsumerHeartbeat> StartDesktopConsumerHeartbeat() {
#if defined(_WIN32) || defined(__APPLE__)
  if constexpr (!kDesktopConsumerHeartbeatEnabled) {
    return nullptr;
  }
  absl::StatusOr<PlatformRegistration> registration =
      MakePlatformRegistration(Environ::GetEnv("GRIMODEX_IME_ROOT"));
  if (!registration.ok()) {
    LOG(ERROR) << "Failed to initialize Grimodex desktop consumer: "
               << registration.status();
    return nullptr;
  }
  return std::make_unique<DesktopConsumerHeartbeatImpl>(
      std::move(*registration));
#else
  return nullptr;
#endif
}

absl::Status UnregisterDesktopConsumer() {
#if defined(_WIN32) || defined(__APPLE__)
  if constexpr (!kDesktopConsumerHeartbeatEnabled) {
    return absl::OkStatus();
  }
  // An installer must not inherit the runtime-only test override and delete a
  // record outside the canonical per-user Protocol v1 root.
  absl::StatusOr<PlatformRegistration> registration =
      MakePlatformRegistration(/*override_root=*/"");
  if (!registration.ok()) {
    return registration.status();
  }
  return registration->registrar->Unregister(
      registration->metadata.consumer_id);
#else
  return absl::UnimplementedError(
      "desktop consumer registration is unsupported on this platform");
#endif
}

absl::Status UnregisterWindowsDesktopConsumerForAppData(
    absl::string_view app_data_directory) {
#if defined(_WIN32)
  if constexpr (!kDesktopConsumerHeartbeatEnabled) {
    return absl::OkStatus();
  }
  const absl::StatusOr<std::string> root =
      ResolveWindowsProtocolV1Root(/*override_root=*/"",
                                   app_data_directory);
  if (!root.ok()) {
    return root.status();
  }
  return WindowsConsumerFileRegistrar(*root).Unregister(kTsfConsumerId);
#else
  static_cast<void>(app_data_directory);
  return absl::UnimplementedError(
      "Windows desktop consumer removal is unsupported on this platform");
#endif
}

}  // namespace mozc::grimodex
