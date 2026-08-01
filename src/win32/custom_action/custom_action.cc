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

#include "win32/custom_action/custom_action.h"

// clang-format off
#include <windows.h>
#include <atlbase.h>
#include <msiquery.h>
#include <tlhelp32.h>
#include <wow64apiset.h>
// clang-format on

#include <cstdarg>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "base/const.h"
#include "base/process.h"
#include "base/system_util.h"
#include "base/url.h"
#include "base/version.h"
#include "base/win32/scoped_com.h"
#include "base/win32/wide_char.h"
#include "base/win32/win_sandbox.h"
#include "base/win32/win_util.h"
#include "client/client.h"
#include "client/client_interface.h"
#include "config/config_handler.h"
#ifndef GOOGLE_JAPANESE_INPUT_BUILD
#include "grimodex/desktop_consumer_heartbeat.h"
#include "ipc/named_event.h"
#endif  // !GOOGLE_JAPANESE_INPUT_BUILD
#include "renderer/renderer_client.h"
#include "win32/base/input_dll.h"
#include "win32/base/omaha_util.h"
#include "win32/base/tsf_profile.h"
#include "win32/base/tsf_registrar.h"
#include "win32/base/uninstall_helper.h"
#include "win32/cache_service/cache_service_manager.h"
#include "win32/custom_action/resource.h"

#if !defined(NDEBUG)
#include <atlstr.h>
#endif  // !NDEBUG

#ifdef _DEBUG
#define DEBUG_BREAK_FOR_DEBUGGER()                                      \
  ::OutputDebugStringA(                                                 \
      (mozc::Version::GetMozcVersion() + ": " + __FUNCTION__).c_str()); \
  if (::IsDebuggerPresent()) {                                          \
    __debugbreak();                                                     \
  }
#else  // _DEBUG
#define DEBUG_BREAK_FOR_DEBUGGER()
#endif  // _DEBUG

namespace {

using mozc::win32::OmahaUtil;

HMODULE g_module = nullptr;

std::wstring GetMozcComponentPath(const absl::string_view filename) {
  return mozc::win32::Utf8ToWide(
      absl::StrCat(mozc::SystemUtil::GetServerDirectory(), "\\", filename));
}

bool StringEqualsIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  return ::CompareStringOrdinal(
             lhs.data(),
             static_cast<int>(lhs.size()),
             rhs.data(),
             static_cast<int>(rhs.size()),
             TRUE) == CSTR_EQUAL;
}

std::wstring GetProcessImagePath(HANDLE process) {
  wchar_t path[MAX_PATH] = {};
  DWORD size = std::size(path);
  if (!::QueryFullProcessImageNameW(process, 0, path, &size)) {
    return L"";
  }
  return std::wstring(path, size);
}

std::wstring GetProcessImagePath(DWORD process_id) {
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 process_id);
  if (process == nullptr) {
    return L"";
  }
  const std::wstring path = GetProcessImagePath(process);
  ::CloseHandle(process);
  return path;
}

bool CollectMozcProcessIdsByImageName(const wchar_t* image_name,
                                      const char* mozc_filename,
                                      std::vector<DWORD>* process_ids) {
  process_ids->clear();
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  const std::wstring expected_path = GetMozcComponentPath(mozc_filename);

  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);

  if (!::Process32FirstW(snapshot, &entry)) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(snapshot);
    return error == ERROR_NO_MORE_FILES;
  }

  bool result = true;
  do {
    if (!StringEqualsIgnoreCase(entry.szExeFile, image_name)) {
      continue;
    }

    const std::wstring actual_path = GetProcessImagePath(entry.th32ProcessID);
    if (actual_path.empty()) {
      // A process with the exact product image name could not be attributed.
      // Fail closed instead of declaring the installed server absent.
      result = false;
      break;
    }
    if (!StringEqualsIgnoreCase(actual_path, expected_path)) {
      continue;
    }
    process_ids->push_back(entry.th32ProcessID);
  } while (::Process32NextW(snapshot, &entry));

  if (result && ::GetLastError() != ERROR_NO_MORE_FILES) {
    result = false;
  }
  ::CloseHandle(snapshot);
  return result;
}

bool TerminateMozcProcessByImageName(const wchar_t* image_name,
                                     const char* mozc_filename) {
  // Repeat enumeration because one product process can be starting while an
  // older instance is being stopped.  Success requires two consecutive exact
  // snapshots with no installed-image process.
  const std::wstring expected_path = GetMozcComponentPath(mozc_filename);
  constexpr int kMaximumAttempts = 5;
  for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    std::vector<DWORD> process_ids;
    if (!CollectMozcProcessIdsByImageName(image_name, mozc_filename,
                                          &process_ids)) {
      return false;
    }
    if (process_ids.empty()) {
      ::Sleep(100);
      if (!CollectMozcProcessIdsByImageName(image_name, mozc_filename,
                                            &process_ids)) {
        return false;
      }
      if (process_ids.empty()) {
        return true;
      }
    }

    for (const DWORD process_id : process_ids) {
      HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                         PROCESS_TERMINATE | SYNCHRONIZE,
                                     FALSE, process_id);
      if (process == nullptr) {
        return false;
      }
      // Recheck the image through the same handle used for termination so PID
      // reuse cannot redirect this uninstall action to another process.
      const std::wstring actual_path = GetProcessImagePath(process);
      if (actual_path.empty() &&
          ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        ::CloseHandle(process);
        continue;
      }
      if (actual_path.empty() ||
          !StringEqualsIgnoreCase(actual_path, expected_path)) {
        ::CloseHandle(process);
        return false;
      }
      if (!::TerminateProcess(process, 0) &&
          ::WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
        ::CloseHandle(process);
        return false;
      }
      const DWORD wait_result = ::WaitForSingleObject(process, 5000);
      ::CloseHandle(process);
      if (wait_result != WAIT_OBJECT_0) {
        return false;
      }
    }
  }
  return false;
}

bool ShutdownZenzRuntimeProcesses() {
  const bool scorer_stopped = TerminateMozcProcessByImageName(
      L"mozc_zenz_scorer.exe", "mozc_zenz_scorer.exe");
  const bool llama_stopped = TerminateMozcProcessByImageName(
      L"llama-server.exe", "llama-server.exe");
  return scorer_stopped && llama_stopped;
}

bool StopMozcServer() {
  const std::wstring image_name =
      mozc::win32::Utf8ToWide(mozc::kMozcServerName);
  return TerminateMozcProcessByImageName(image_name.c_str(),
                                         mozc::kMozcServerName);
}

bool StopMozcBroker() {
#ifndef GOOGLE_JAPANESE_INPUT_BUILD
  // Ask the medium-integrity heartbeat owner to stop cleanly first. Exact-path
  // termination below is the fail-closed fallback and also proves that a
  // racing/stale installed broker cannot republish after uninstall cleanup.
  mozc::NamedEventNotifier notifier(
      mozc::kGrimodexConsumerBrokerShutdownEvent);
  if (notifier.IsAvailable()) {
    notifier.Notify();
    ::Sleep(100);
  }
#endif  // !GOOGLE_JAPANESE_INPUT_BUILD
  const std::wstring image_name =
      mozc::win32::Utf8ToWide(mozc::kMozcBroker);
  return TerminateMozcProcessByImageName(image_name.c_str(),
                                         mozc::kMozcBroker);
}

// Retrieves the value for an installer property.
// Returns an empty string if a property corresponding to |name| is not found or
// error occurs.
std::wstring GetProperty(MSIHANDLE msi, std::wstring_view name) {
  DWORD num_buf = 0;
  // Obtains the size of the property's string, without null termination.
  // Note: |MsiGetProperty()| requires non-null writable buffer.
  std::wstring buf;
  UINT result =
      MsiGetProperty(msi, std::wstring(name).c_str(), buf.data(), &num_buf);
  if (result != ERROR_MORE_DATA) {
    return L"";
  }

  buf.resize(num_buf);
  // add 1 for null termination
  num_buf += 1;
  result =
      MsiGetProperty(msi, std::wstring(name).c_str(), buf.data(), &num_buf);
  if (result != ERROR_SUCCESS) {
    return L"";
  }

  return buf;
}

bool SetProperty(MSIHANDLE msi, const std::wstring_view name,
                 const std::wstring_view value) {
  if (MsiSetProperty(msi, std::wstring(name).c_str(),
                     std::wstring(value).c_str()) != ERROR_SUCCESS) {
    return false;
  }
  return true;
}

void LogInstallerInfo(MSIHANDLE msi_handle, std::wstring_view message) {
  PMSIHANDLE record = MsiCreateRecord(1);
  if (!record) {
    return;
  }
  MsiRecordSetStringW(record, 0, std::wstring(message).c_str());
  MsiProcessMessage(msi_handle, INSTALLMESSAGE_INFO, record);
}

std::wstring FormatMessageByResourceId(int resourceID, ...) {
  wchar_t format_message[4096];
  {
    const int length = ::LoadString(g_module, resourceID, format_message,
                                    std::size(format_message));
    if (length <= 0 || std::size(format_message) <= length) {
      return L"";
    }
  }
  va_list va_args = nullptr;
  va_start(va_args, resourceID);

  wchar_t buffer[4096];  // should be less than 64KB.
  // TODO(yukawa): Use message table instead of string table.
  {
    const DWORD num_chars =
        ::FormatMessage(FORMAT_MESSAGE_FROM_STRING, format_message, 0, 0,
                        &buffer[0], std::size(buffer), &va_args);
    va_end(va_args);

    if (num_chars == 0 || num_chars >= std::size(buffer)) {
      return L"";
    }
  }

  return buffer;
}

std::wstring GetVersionHeader() {
  return FormatMessageByResourceId(IDS_FORMAT_VERSION_INFO,
                                   mozc::Version::GetMozcVersionW().c_str());
}

bool WriteOmahaErrorById(int resource_id) {
  wchar_t buffer[4096];
  const int length =
      ::LoadString(g_module, resource_id, buffer, std::size(buffer));
  if (length <= 0 || std::size(buffer) <= length) {
    return false;
  }

  return OmahaUtil::WriteOmahaError(buffer, GetVersionHeader());
}

template <size_t num_elements>
bool WriteOmahaError(const wchar_t (&function)[num_elements], int line) {
#if !defined(NDEBUG)
  ATL::CStringW log;
  log.Format(L"%s: %s; %s(%d)", L"WriteOmahaError: ",
             mozc::Version::GetMozcVersionW().c_str(), function, line);
  ::OutputDebugStringW(log);
#endif  // !defined(NDEBUG)
  const std::wstring& message =
      FormatMessageByResourceId(IDS_FORMAT_FUNCTION_AND_LINE, function, line);
  return OmahaUtil::WriteOmahaError(message, GetVersionHeader());
}

// Compose an error message based on the function name and line number.
// This message will be displayed by Omaha meta installer on the error
// dialog.
#define LOG_ERROR_FOR_OMAHA() WriteOmahaError(_T(__FUNCTION__), __LINE__)

std::wstring QuoteCommandLineArg(std::wstring_view arg) {
  std::wstring result;
  result.reserve(arg.size() + 2);
  result.push_back(L'"');

  size_t backslash_count = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslash_count;
      continue;
    }

    if (ch == L'"') {
      result.append(backslash_count * 2 + 1, L'\\');
      result.push_back(ch);
      backslash_count = 0;
      continue;
    }

    result.append(backslash_count, L'\\');
    backslash_count = 0;
    result.push_back(ch);
  }

  result.append(backslash_count * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::wstring GetSystemNetshPath() {
  wchar_t system_dir[MAX_PATH] = {};
  const UINT length = ::GetSystemDirectoryW(system_dir, std::size(system_dir));
  if (length == 0 || length >= std::size(system_dir)) {
    return L"netsh.exe";
  }
  return ::mozc::win32::StrCatW(system_dir, L"\\netsh.exe");
}

bool RunNetshCommand(const std::vector<std::wstring>& args) {
  std::wstring command_line = QuoteCommandLineArg(GetSystemNetshPath());
  for (const std::wstring& arg : args) {
    command_line.push_back(L' ');
    command_line.append(QuoteCommandLineArg(arg));
  }

  std::vector<wchar_t> mutable_command_line(command_line.begin(),
                                            command_line.end());
  mutable_command_line.push_back(L'\0');

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);

  PROCESS_INFORMATION process_info = {};

  if (!::CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup_info, &process_info)) {
    return false;
  }

  ::WaitForSingleObject(process_info.hProcess, INFINITE);

  DWORD exit_code = 1;
  const bool got_exit_code =
      ::GetExitCodeProcess(process_info.hProcess, &exit_code) != FALSE;

  ::CloseHandle(process_info.hThread);
  ::CloseHandle(process_info.hProcess);

  return got_exit_code && exit_code == 0;
}

struct MozcFirewallRule {
  const wchar_t* name;
  const char* filename;
};

constexpr MozcFirewallRule kMozcOfflineFirewallRules[] = {
    {L"Mozkey IbG Offline - Block mozc_server outbound", "mozc_server.exe"},
    {L"Mozkey IbG Offline - Block mozc_tool outbound", "mozc_tool.exe"},
    {L"Mozkey IbG Offline - Block mozc_renderer outbound", "mozc_renderer.exe"},
    {L"Mozkey IbG Offline - Block mozc_broker outbound", "mozc_broker.exe"},
    {L"Mozkey IbG Offline - Block mozc_cache_service outbound",
     "mozc_cache_service.exe"},
};

void DeleteFirewallRule(std::wstring_view rule_name) {
  // Ignore failures. The rule may not exist yet.
  RunNetshCommand({
      L"advfirewall",
      L"firewall",
      L"delete",
      L"rule",
      ::mozc::win32::StrCatW(L"name=", rule_name),
  });
}

void AddFirewallRule(std::wstring_view rule_name, std::wstring_view program) {
  // Make the operation idempotent. This avoids duplicate rules after repair or
  // upgrade.
  DeleteFirewallRule(rule_name);

  RunNetshCommand({
      L"advfirewall",
      L"firewall",
      L"add",
      L"rule",
      ::mozc::win32::StrCatW(L"name=", rule_name),
      L"dir=out",
      L"action=block",
      ::mozc::win32::StrCatW(L"program=", program),
      L"enable=yes",
      L"profile=any",
  });
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD ul_reason_for_call,
                      LPVOID reserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      g_module = module;
      break;
    case DLL_PROCESS_DETACH:
      g_module = nullptr;
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
  }

  return TRUE;
}

// [Return='ignore']
UINT __stdcall EnsureAllApplicationPackagesPermisssions(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  if (!mozc::WinSandbox::EnsureAllApplicationPackagesPermisssion(
          GetMozcComponentPath(mozc::kMozcServerName),
          mozc::WinSandbox::AppContainerVisibilityType::kProgramFiles)) {
    return ERROR_INSTALL_FAILURE;
  }
  if (!mozc::WinSandbox::EnsureAllApplicationPackagesPermisssion(
          GetMozcComponentPath(mozc::kMozcRenderer),
          mozc::WinSandbox::AppContainerVisibilityType::kProgramFiles)) {
    return ERROR_INSTALL_FAILURE;
  }
  if (!mozc::WinSandbox::EnsureAllApplicationPackagesPermisssion(
          GetMozcComponentPath(mozc::kMozcTIP32),
          mozc::WinSandbox::AppContainerVisibilityType::kProgramFiles)) {
    return ERROR_INSTALL_FAILURE;
  }
  if (!mozc::WinSandbox::EnsureAllApplicationPackagesPermisssion(
          GetMozcComponentPath(mozc::kMozcTIP64),
          mozc::WinSandbox::AppContainerVisibilityType::kProgramFiles)) {
    return ERROR_INSTALL_FAILURE;
  }
  return ERROR_SUCCESS;
}

// [Return='ignore']
UINT __stdcall OpenUninstallSurveyPage(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  mozc::Process::OpenBrowser(
      mozc::url::GetUninstallationSurveyUrl(mozc::Version::GetMozcVersion()));
  return ERROR_SUCCESS;
}

UINT __stdcall ShutdownServer(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();

  const bool broker_stopped = StopMozcBroker();
  ShutdownZenzRuntimeProcesses();

  std::unique_ptr<mozc::client::ClientInterface> server_client(
      mozc::client::ClientFactory::NewClient());
  if (server_client->PingServer()) {
    if (!server_client->Shutdown()) {
      // This is not fatal as Windows Installer can replace executables even
      // when they still are running. Just log error then go ahead.
      LOG_ERROR_FOR_OMAHA();
    }
  }

  std::unique_ptr<mozc::renderer::RendererClient> renderer_client =
      mozc::renderer::RendererClient::Create();
  if (!renderer_client->Shutdown(true)) {
    // This is not fatal as Windows Installer can replace executables even when
    // they are still running. Just log error then go ahead.
    LOG_ERROR_FOR_OMAHA();
  }

  const bool server_stopped = StopMozcServer();
  // Stop helpers only after the exact server is absent, so it cannot launch a
  // replacement scorer between the final helper scan and server termination.
  const bool zenz_stopped = ShutdownZenzRuntimeProcesses();

  if (!broker_stopped || !zenz_stopped || !server_stopped) {
    LogInstallerInfo(
        msi_handle,
        L"Mozkey IbG could not prove that all installed runtime processes stopped.");
    return ERROR_INSTALL_FAILURE;
  }
  return ERROR_SUCCESS;
}

// [Return='ignore']
UINT __stdcall RestoreUserIMEEnvironment(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  const bool result =
      mozc::win32::UninstallHelper::RestoreUserIMEEnvironmentMain();
  return result ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

UINT __stdcall UnregisterGrimodexConsumer(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
#ifdef GOOGLE_JAPANESE_INPUT_BUILD
  static_cast<void>(msi_handle);
  return ERROR_SUCCESS;
#else   // GOOGLE_JAPANESE_INPUT_BUILD
  const std::wstring app_data = GetProperty(msi_handle, L"CustomActionData");
  if (!StopMozcBroker() || !StopMozcServer()) {
    // The broker owns the tsf-mozkey-ibg heartbeat refresh loop. Never remove
    // the record while the exact installed broker or legacy server may still
    // republish it.
    LogInstallerInfo(
        msi_handle,
        L"Mozkey IbG kept its Grimodex consumer heartbeat because the installed "
        L"heartbeat owner could not be stopped and proven absent.");
    return ERROR_INSTALL_FAILURE;
  }
  const absl::Status status =
      app_data.empty()
          ? absl::FailedPreconditionError(
                "MSI AppDataFolder is unavailable for Grimodex cleanup")
          : mozc::grimodex::UnregisterWindowsDesktopConsumerForAppData(
                mozc::win32::WideToUtf8(app_data));
  if (status.ok()) {
    return ERROR_SUCCESS;
  }

  // A secure registrar failure must never fall back to broad recursive
  // removal. The checked MSI action reports the failure to the user.
  LogInstallerInfo(msi_handle, mozc::win32::Utf8ToWide(absl::StrCat(
                                   "Mozkey IbG could not remove its Grimodex "
                                   "consumer heartbeat: ",
                                   status.ToString())));
  return ERROR_INSTALL_FAILURE;
#endif  // GOOGLE_JAPANESE_INPUT_BUILD
}

// [Return='ignore']
// Hides the cancel button on a progress dialog shown by the installer shows.
// Please see the following page for details.
// http://msdn.microsoft.com/en-us/library/aa368791(VS.85).aspx
UINT __stdcall HideCancelButton(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  PMSIHANDLE record = MsiCreateRecord(2);
  if (!record) {
    return ERROR_INSTALL_FAILURE;
  }
  if ((ERROR_SUCCESS != MsiRecordSetInteger(record, 1, 2) ||
       ERROR_SUCCESS != MsiRecordSetInteger(record, 2, 0))) {
    return ERROR_INSTALL_FAILURE;
  }
  MsiProcessMessage(msi_handle, INSTALLMESSAGE_COMMONDATA, record);
  return ERROR_SUCCESS;
}

UINT __stdcall InitialInstallation(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();

  // Write a general error message in case any unexpected error occurs.
  WriteOmahaErrorById(IDS_UNEXPECTED_ERROR);

  return ERROR_SUCCESS;
}

UINT __stdcall InitialInstallationCommit(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();

  // Set error code 0, which means success.
  OmahaUtil::ClearOmahaError();
  return ERROR_SUCCESS;
}

UINT __stdcall EnableTipProfile(MSIHANDLE msi_handle) {
  bool is_service = false;
  if (::mozc::WinUtil::IsServiceAccount(&is_service) && is_service) {
    // Do nothing if this is a service account.
    return ERROR_SUCCESS;
  }

  wchar_t clsid[64] = {};
  if (!::StringFromGUID2(::mozc::win32::TsfProfile::GetTextServiceGuid(), clsid,
                         std::size(clsid))) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }

  wchar_t profile_id[64] = {};
  if (!::StringFromGUID2(::mozc::win32::TsfProfile::GetProfileGuid(),
                         profile_id, std::size(profile_id))) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }

  // 0x0411 == MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN)
  const auto desc = ::mozc::win32::StrCatW(L"0x0411:", clsid, profile_id);

  if (!::InstallLayoutOrTip(desc.c_str(), 0)) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }

  return ERROR_SUCCESS;
}

UINT __stdcall FixupConfigFilePermission(MSIHANDLE msi_handle) {
  bool is_service = false;
  if (::mozc::WinUtil::IsServiceAccount(&is_service) && is_service) {
    // Do nothing if this is a service account.
    return ERROR_SUCCESS;
  }

  // Check the file permission of "config1.db" if exists to ensure that
  // "ALL APPLICATION PACKAGES" have read access to it.
  // See https://github.com/google/mozc/issues/1076 for details.
  ::mozc::config::ConfigHandler::FixupFilePermission();

  // Return always ERROR_SUCCESS regardless of the result, as not being able
  // to fixup the permission is not problematic enough to block installation
  // and upgrading.
  return ERROR_SUCCESS;
}

UINT __stdcall SaveCustomActionData(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  // store the CHANNEL value specified in the command line argument for
  // WriteApValue.
  const std::wstring channel = GetProperty(msi_handle, L"CHANNEL");
  if (!channel.empty()) {
    if (!SetProperty(msi_handle, L"WriteApValue", channel)) {
      LOG_ERROR_FOR_OMAHA();
      return ERROR_INSTALL_FAILURE;
    }
  }

  // store the original ap value for WriteApValueRollback.
  const std::wstring ap_value = OmahaUtil::ReadChannel();
  if (!SetProperty(msi_handle, L"WriteApValueRollback", ap_value.c_str())) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }

  // store the current settings of the cache service.
  std::wstring backup;
  if (!mozc::CacheServiceManager::BackupStateAsString(&backup)) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }
  if (!SetProperty(msi_handle, L"RestoreServiceState", backup.c_str())) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }
  if (!SetProperty(msi_handle, L"RestoreServiceStateRollback",
                   backup.c_str())) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }
  return ERROR_SUCCESS;
}

// [Return='ignore']
// This function is used for the following CustomActions:
// "RestoreServiceState" and "RestoreServiceStateRollback"
UINT __stdcall RestoreServiceState(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  const std::wstring& backup = GetProperty(msi_handle, L"CustomActionData");
  if (!mozc::CacheServiceManager::RestoreStateFromString(backup)) {
    return ERROR_INSTALL_FAILURE;
  }

  return ERROR_SUCCESS;
}

// [Return='ignore']
UINT __stdcall StopCacheService(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  if (!mozc::CacheServiceManager::EnsureServiceStopped()) {
    return ERROR_INSTALL_FAILURE;
  }

  return ERROR_SUCCESS;
}

// [Return='ignore']
UINT __stdcall InstallMozcFirewallRules(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();

  for (const MozcFirewallRule& rule : kMozcOfflineFirewallRules) {
    AddFirewallRule(rule.name, GetMozcComponentPath(rule.filename));
  }

  // Firewall rule installation is best-effort. Do not block MSI installation
  // when enterprise policy or local security settings reject rule creation.
  return ERROR_SUCCESS;
}

// [Return='ignore']
UINT __stdcall RemoveMozcFirewallRules(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();

  for (const MozcFirewallRule& rule : kMozcOfflineFirewallRules) {
    DeleteFirewallRule(rule.name);
  }

  // Firewall rule removal is best-effort. Do not block MSI uninstall.
  return ERROR_SUCCESS;
}

UINT __stdcall WriteApValue(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  const std::wstring channel = GetProperty(msi_handle, L"CustomActionData");
  if (channel.empty()) {
    // OK. Does not change ap value when CustomActionData is not found.
    return ERROR_SUCCESS;
  }

  const bool result = OmahaUtil::WriteChannel(channel);
  if (!result) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }
  return ERROR_SUCCESS;
}

UINT __stdcall WriteApValueRollback(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  const std::wstring ap_value = GetProperty(msi_handle, L"CustomActionData");
  if (ap_value.empty()) {
    // The ap value did not originally exist so attempt to delete the value.
    if (!OmahaUtil::ClearChannel()) {
      LOG_ERROR_FOR_OMAHA();
      return ERROR_INSTALL_FAILURE;
    }
    return ERROR_SUCCESS;
  }

  // Restore the original ap value.
  if (!OmahaUtil::WriteChannel(ap_value)) {
    LOG_ERROR_FOR_OMAHA();
    return ERROR_INSTALL_FAILURE;
  }
  return ERROR_SUCCESS;
}

UINT __stdcall RegisterTIP(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  mozc::ScopedCOMInitializer com_initializer;
  HRESULT result = S_OK;

  // Register 64-bit TIP COM server.
  // Unlike 32-bit TIP DLL, which is always x86, the expected 64-bit TIP DLL
  // can be x64 or ARM64X depending on the target environment.
  USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
  USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
  result = IsWow64Process2(::GetCurrentProcess(), &process_machine,
                           &native_machine);
  const bool is_arm64_machine =
      SUCCEEDED(result) && native_machine == IMAGE_FILE_MACHINE_ARM64;
  const std::wstring tip64_path = GetMozcComponentPath(
      is_arm64_machine ? mozc::kMozcTIP64X : mozc::kMozcTIP64);

  result = mozc::win32::TsfRegistrar::RegisterCOMServer(
      tip64_path.c_str(), tip64_path.length(),
      mozc::win32::COMServerBitness::k64bit);
  if (FAILED(result)) {
    LOG_ERROR_FOR_OMAHA();
    UnregisterTIP(msi_handle);
    return ERROR_INSTALL_FAILURE;
  }

  // Register 32-bit TIP COM server.
  const std::wstring tip32_path = GetMozcComponentPath(mozc::kMozcTIP32);
  result = mozc::win32::TsfRegistrar::RegisterCOMServer(
      tip32_path.c_str(), tip32_path.length(),
      mozc::win32::COMServerBitness::k32bit);
  if (FAILED(result)) {
    LOG_ERROR_FOR_OMAHA();
    UnregisterTIP(msi_handle);
    return ERROR_INSTALL_FAILURE;
  }

  // Register profiles and categories.
  // The path here is to retrieve Win32 resources such as icon and product name,
  // which does not need to match the native CPU architecture. Here we use
  // 32-bit TIP DLL as it is always installed even on an ARM64 target.
  result = mozc::win32::TsfRegistrar::RegisterProfiles(tip32_path);
  if (FAILED(result)) {
    LOG_ERROR_FOR_OMAHA();
    UnregisterTIP(msi_handle);
    return ERROR_INSTALL_FAILURE;
  }

  result = mozc::win32::TsfRegistrar::RegisterCategories();
  if (FAILED(result)) {
    LOG_ERROR_FOR_OMAHA();
    UnregisterTIP(msi_handle);
    return ERROR_INSTALL_FAILURE;
  }

  return ERROR_SUCCESS;
}

UINT __stdcall RegisterTIPRollback(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  return UnregisterTIP(msi_handle);
}

// [Return='ignore']
UINT __stdcall UnregisterTIP(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  mozc::ScopedCOMInitializer com_initializer;

  mozc::win32::TsfRegistrar::UnregisterCategories();
  mozc::win32::TsfRegistrar::UnregisterProfiles();
  mozc::win32::TsfRegistrar::UnregisterCOMServer(
      mozc::win32::COMServerBitness::k64bit);
  mozc::win32::TsfRegistrar::UnregisterCOMServer(
      mozc::win32::COMServerBitness::k32bit);

  return ERROR_SUCCESS;
}

// [Return='ignore']
UINT __stdcall UnregisterTIPRollback(MSIHANDLE msi_handle) {
  DEBUG_BREAK_FOR_DEBUGGER();
  return RegisterTIP(msi_handle);
}
