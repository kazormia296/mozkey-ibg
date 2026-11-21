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

#import "Uninstaller.h"

#include <Security/Security.h>

#import "DialogsController.h"

#include <libproc.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <set>
#include <string>
#include <vector>

#include "base/mac/mac_util.h"
#include "base/url.h"
#include "base/version.h"
#ifndef GOOGLE_JAPANESE_INPUT_BUILD
#include "grimodex/desktop_consumer_heartbeat.h"
#endif  // GOOGLE_JAPANESE_INPUT_BUILD

namespace {

NSURL *kUninstallSurveyUrl = nil;
NSString *kUninstallerScriptPath = nil;

bool GetPrevilegeRights(AuthorizationRef *auth) {
  if (auth == nullptr) {
    return false;
  }
  *auth = nullptr;
  OSStatus status;
  AuthorizationFlags authFlags = kAuthorizationFlagDefaults;

  status = AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment, authFlags, auth);
  if (status != errAuthorizationSuccess) {
    return false;
  }

  AuthorizationItem item = {kAuthorizationRightExecute, 0, nullptr, 0};
  AuthorizationRights rights = {1, &item};

  authFlags = kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed |
              kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights;
  status = AuthorizationCopyRights(*auth, &rights, nullptr, authFlags, nullptr);

  return status == errAuthorizationSuccess;
}

bool OpenUninstallSurvey() { return [[NSWorkspace sharedWorkspace] openURL:kUninstallSurveyUrl]; }

bool DeleteFiles(const AuthorizationRef &auth) {
  const char *kRmPath = "/bin/rm";
  const char *rmArgs[] = {"-rf", nullptr, nullptr};
#ifdef GOOGLE_JAPANESE_INPUT_BUILD
  const char *kRemovePaths[] = {
      "/Library/Input Methods/GoogleJapaneseInput.app",
      "/Library/LaunchAgents/com.google.inputmethod.Japanese.Converter.plist",
      "/Library/LaunchAgents/com.google.inputmethod.Japanese.Renderer.plist",
      "/Applications/GoogleJapaneseInput.localized", nullptr};
#else   // GOOGLE_JAPANESE_INPUT_BUILD
  const char *kRemovePaths[] = {
      "/Library/Input Methods/MozkeyIbG.app",
      "/Library/LaunchAgents/io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.Converter.plist",
      "/Library/LaunchAgents/io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.Renderer.plist",
      "/Applications/MozkeyIbG",
      nullptr};
#endif  // GOOGLE_JAPANESE_INPUT_BUILD
  for (int i = 0; kRemovePaths[i] != nullptr; ++i) {
    rmArgs[1] = kRemovePaths[i];
    OSStatus status = AuthorizationExecuteWithPrivileges(
        auth, kRmPath, kAuthorizationFlagDefaults, const_cast<char *const *>(rmArgs), nullptr);
    if (status != errAuthorizationSuccess) {
      NSLog(@"Failed to remove path: %s", kRemovePaths[i]);
      return false;
    }
  }
  return true;
}

#ifndef GOOGLE_JAPANESE_INPUT_BUILD
int RunTask(NSString *launch_path, NSArray<NSString *> *arguments) {
  @try {
    NSTask *task =
        [NSTask launchedTaskWithLaunchPath:launch_path arguments:arguments];
    [task waitUntilExit];
    return [task terminationStatus];
  } @catch (NSException *exception) {
    NSLog(@"Failed to run %@: %@", launch_path, exception);
    return -1;
  }
}

bool RunTaskWithOutput(NSString *launch_path, NSArray<NSString *> *arguments,
                       int *status, std::string *output) {
  if (status == nullptr || output == nullptr) {
    return false;
  }
  @try {
    NSPipe *pipe = [NSPipe pipe];
    NSTask *task = [[NSTask alloc] init];
    [task setLaunchPath:launch_path];
    [task setArguments:arguments];
    [task setStandardOutput:pipe];
    [task setStandardError:pipe];
    [task launch];
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    [task waitUntilExit];
    *status = [task terminationStatus];
    output->assign(static_cast<const char *>([data bytes]), [data length]);
    return true;
  } @catch (NSException *exception) {
    NSLog(@"Failed to run %@: %@", launch_path, exception);
    return false;
  }
}

bool ListUserPids(uid_t uid, std::vector<pid_t> *pids) {
  if (pids == nullptr) {
    return false;
  }
  pids->clear();
  const int required_bytes = proc_listpids(PROC_UID_ONLY, uid, nullptr, 0);
  if (required_bytes <= 0) {
    NSLog(@"Failed to enumerate the current user's processes");
    return false;
  }
  pids->resize(static_cast<size_t>(required_bytes) / sizeof(pid_t) + 32);
  const int populated_bytes =
      proc_listpids(PROC_UID_ONLY, uid, pids->data(),
                    static_cast<int>(pids->size() * sizeof(pid_t)));
  if (populated_bytes <= 0) {
    pids->clear();
    NSLog(@"Failed to read the current user's process list");
    return false;
  }
  pids->resize(static_cast<size_t>(populated_bytes) / sizeof(pid_t));
  return true;
}

bool FindMozcConverterProcesses(uid_t uid, std::vector<pid_t> *converter_pids) {
  if (converter_pids == nullptr) {
    return false;
  }
  converter_pids->clear();
  std::vector<pid_t> pids;
  if (!ListUserPids(uid, &pids)) {
    return false;
  }
  constexpr char kConverterPath[] =
      "/Library/Input Methods/MozkeyIbG.app/Contents/Resources/"
      "MozkeyIbGConverter.app/Contents/MacOS/MozkeyIbGConverter";
  for (const pid_t pid : pids) {
    if (pid <= 0) {
      continue;
    }
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath(pid, path, sizeof(path)) > 0 &&
        std::string(path) == kConverterPath) {
      converter_pids->push_back(pid);
    }
  }
  return true;
}

bool WaitForMozcConverterExit(uid_t uid, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::vector<pid_t> converter_pids;
    if (!FindMozcConverterProcesses(uid, &converter_pids)) {
      return false;
    }
    if (converter_pids.empty()) {
      return true;
    }
    usleep(100 * 1000);
  }
  std::vector<pid_t> converter_pids;
  return FindMozcConverterProcesses(uid, &converter_pids) &&
         converter_pids.empty();
}

bool SignalMozcConverter(uid_t uid, int signal_number) {
  std::vector<pid_t> converter_pids;
  if (!FindMozcConverterProcesses(uid, &converter_pids)) {
    return false;
  }
  for (const pid_t pid : converter_pids) {
    if (kill(pid, signal_number) != 0 && errno != ESRCH) {
      NSLog(@"Failed to signal MozkeyIbGConverter pid %d", pid);
      return false;
    }
  }
  return true;
}

bool IsConverterLaunchAgentAbsent(NSString *domain) {
  constexpr char kNotFound[] = "Could not find service";
  NSString *service = [NSString
      stringWithFormat:@"%@/%@", domain,
                       @"io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.Converter"];
  int status = 0;
  std::string output;
  if (!RunTaskWithOutput(@"/bin/launchctl", @[@"print", service], &status,
                         &output)) {
    return false;
  }
  // launchctl uses 113 for a missing service.  Require its specific diagnostic
  // too so an unrelated query failure cannot be mistaken for absence.
  return status == 113 && output.find(kNotFound) != std::string::npos;
}

bool StopMozcConverter() {
  const uid_t uid = getuid();
  NSString *domain = [NSString stringWithFormat:@"gui/%u", uid];
  NSString *plist =
      @"/Library/LaunchAgents/io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.Converter.plist";

  // Remove the job before terminating the process so launchd cannot respawn
  // MozkeyIbGConverter between the process-exit check and heartbeat removal.
  const int bootout_status =
      RunTask(@"/bin/launchctl", @[@"bootout", domain, plist]);
  if (!IsConverterLaunchAgentAbsent(domain)) {
    NSLog(@"MozkeyIbGConverter LaunchAgent remains loaded after bootout (status %d)",
          bootout_status);
    return false;
  }

  if (!SignalMozcConverter(uid, SIGTERM)) {
    return false;
  }
  if (WaitForMozcConverterExit(uid, /*attempts=*/50)) {
    return true;
  }

  if (!SignalMozcConverter(uid, SIGKILL)) {
    return false;
  }
  if (WaitForMozcConverterExit(uid, /*attempts=*/20)) {
    return true;
  }

  NSLog(@"MozkeyIbGConverter is still running; refusing heartbeat removal");
  return false;
}

bool FindMozkeyZenzRuntimeProcessGroups(uid_t uid,
                                       std::set<pid_t> *process_groups) {
  if (process_groups == nullptr) {
    return false;
  }
  process_groups->clear();

  std::vector<pid_t> pids;
  if (!ListUserPids(uid, &pids)) {
    return false;
  }

  // Exact Mozkey IbG executable paths only; never a basename match, so an
  // upstream Mozkey runtime running side by side is not signalled.  The
  // ZenzRuntime entries are the bundled runtime layout; the Converter.app
  // entries are the layout of Mozkey IbG v0.10.x and earlier.
  static constexpr const char *kRuntimePaths[] = {
      "/Library/Input Methods/MozkeyIbG.app/Contents/Resources/"
      "ZenzRuntime/mozc_zenz_scorer",
      "/Library/Input Methods/MozkeyIbG.app/Contents/Resources/"
      "ZenzRuntime/mozc_zenz_guard",
      "/Library/Input Methods/MozkeyIbG.app/Contents/Resources/"
      "ZenzRuntime/llama-server",
      "/Library/Input Methods/MozkeyIbG.app/Contents/Resources/"
      "MozkeyIbGConverter.app/Contents/Resources/mozc_zenz_scorer",
      "/Library/Input Methods/MozkeyIbG.app/Contents/Resources/"
      "MozkeyIbGConverter.app/Contents/Resources/llama-server",
  };
  const auto is_runtime_path = [](const std::string &path) {
    for (const char *runtime_path : kRuntimePaths) {
      if (path == runtime_path) {
        return true;
      }
    }
    return false;
  };
  const pid_t uninstaller_process_group = getpgrp();
  for (const pid_t pid : pids) {
    if (pid <= 0) {
      continue;
    }
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0 ||
        !is_runtime_path(path)) {
      continue;
    }
    const pid_t process_group = getpgid(pid);
    if (process_group <= 0) {
      if (errno == ESRCH) {
        continue;
      }
      NSLog(@"Failed to resolve Mozkey Zenz process group for pid %d", pid);
      return false;
    }
    if (process_group == uninstaller_process_group) {
      NSLog(@"Refusing to signal the uninstaller process group");
      return false;
    }
    process_groups->insert(process_group);
  }
  return true;
}

bool WaitForMozkeyZenzRuntimeExit(uid_t uid, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::set<pid_t> process_groups;
    if (!FindMozkeyZenzRuntimeProcessGroups(uid, &process_groups)) {
      return false;
    }
    if (process_groups.empty()) {
      return true;
    }
    usleep(100 * 1000);
  }
  std::set<pid_t> process_groups;
  return FindMozkeyZenzRuntimeProcessGroups(uid, &process_groups) &&
         process_groups.empty();
}

bool SignalMozkeyZenzRuntime(uid_t uid, int signal) {
  std::set<pid_t> process_groups;
  if (!FindMozkeyZenzRuntimeProcessGroups(uid, &process_groups)) {
    return false;
  }
  for (const pid_t process_group : process_groups) {
    if (kill(-process_group, signal) != 0 && errno != ESRCH) {
      NSLog(@"Failed to signal Mozkey Zenz process group %d", process_group);
      return false;
    }
  }
  return true;
}

bool StopMozkeyZenzRuntime() {
  const uid_t uid = getuid();
  if (!SignalMozkeyZenzRuntime(uid, SIGTERM)) {
    return false;
  }
  if (WaitForMozkeyZenzRuntimeExit(uid, /*attempts=*/50)) {
    return true;
  }
  if (!SignalMozkeyZenzRuntime(uid, SIGKILL)) {
    return false;
  }
  if (WaitForMozkeyZenzRuntimeExit(uid, /*attempts=*/20)) {
    return true;
  }
  NSLog(@"Mozkey Zenz runtime is still running; refusing bundle removal");
  return false;
}

void UnregisterGrimodexConsumer() {
  const absl::Status status = mozc::grimodex::UnregisterDesktopConsumer();
  if (!status.ok()) {
    // Do not weaken the secure registrar by falling back to a recursive
    // delete.  Log the incomplete cleanup instead of failing the rest of the
    // user-requested uninstall.
    NSLog(@"Failed to remove the Mozkey Grimodex consumer heartbeat: %s",
          status.ToString().c_str());
  }
}
#endif  // GOOGLE_JAPANESE_INPUT_BUILD

bool UnregisterKeystoneTicket(const AuthorizationRef &auth) {
  const char *kKsadminPath = "/Library/Google/GoogleSoftwareUpdate/"
                             "GoogleSoftwareUpdate.bundle/Contents/MacOS/ksadmin";
  const char *kKsadminArgs[] = {"--delete", "--productid", "com.google.JapaneseIME", nullptr};
  OSStatus status =
      AuthorizationExecuteWithPrivileges(auth, kKsadminPath, kAuthorizationFlagDefaults,
                                         const_cast<char *const *>(kKsadminArgs), nullptr);
  return status == errAuthorizationSuccess;
}

bool RunReboot(const AuthorizationRef &auth) {
  // TODO(mukai): Use OS-specific API instead of calling reboot command.
  const char *rebootPath = "/sbin/reboot";
  char *args[] = {nullptr};
  OSStatus status = AuthorizationExecuteWithPrivileges(auth, rebootPath, kAuthorizationFlagDefaults,
                                                       args, nullptr);
  return status == errAuthorizationSuccess;
}
}  // namespace

@implementation Uninstaller

+ (void)doUninstall:(DialogsController *)dialogs {
  mozc::MacUtil::RemovePrelauncherLoginItem();

  AuthorizationRef auth = nullptr;

  if (!GetPrevilegeRights(&auth)) {
    [dialogs reportAuthError];
    if (auth != nullptr) {
      AuthorizationFree(auth, kAuthorizationFlagDefaults);
    }
    return;
  }
#ifdef GOOGLE_JAPANESE_INPUT_BUILD
  if (OpenUninstallSurvey() && DeleteFiles(auth) && UnregisterKeystoneTicket(auth)) {
#else   // GOOGLE_JAPANESE_INPUT_BUILD
  // Ordinary shutdown and package replacement intentionally leave this
  // heartbeat.  Only the user-facing uninstaller removes imkit-mozkey-ibg. Stop
  // the launchd job and process first so they cannot republish the heartbeat
  // after the registrar removes it.
  const bool stopped = StopMozcConverter() && StopMozkeyZenzRuntime();
  const bool deleted = stopped && DeleteFiles(auth);
  if (deleted) {
    UnregisterGrimodexConsumer();
  }
  if (deleted) {
#endif  // GOOGLE_JAPANESE_INPUT_BUILD
    if ([dialogs reportUninstallSuccess]) {
      RunReboot(auth);
    }
  } else {
    [dialogs reportUninstallError];
  }

  if (auth != nullptr) {
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
  }
}

+ (void)initializeUninstaller {
  const std::string url = mozc::url::GetUninstallationSurveyUrl(mozc::Version::GetMozcVersion());
  NSString *uninstallUrl = [[NSString alloc] initWithBytes:url.data()
                                                    length:url.size()
                                                  encoding:NSUTF8StringEncoding];
  kUninstallSurveyUrl = [NSURL URLWithString:uninstallUrl];

  kUninstallerScriptPath = [[NSBundle mainBundle] pathForResource:@"uninstaller"
                                                           ofType:@"py"
                                                      inDirectory:nil];
}

@end
