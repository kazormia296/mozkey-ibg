# -*- coding: utf-8 -*-
# Copyright 2010-2021, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Change the file structures of the install files.

tweak_installfer_files.py --input installer.zip --output tweaked_installer.zip
"""

import argparse
import os
import shutil
import stat
import tempfile

from build_tools import util


# Raw Mach-O helpers of the macOS Zenz runtime (Resources/ZenzRuntime).  The
# scorer always starts llama-server through mozc_zenz_guard.
_NESTED_EXECUTABLE_NAMES = frozenset(
    ('llama-server', 'mozc_zenz_scorer', 'mozc_zenz_guard')
)
_ZENZ_RUNTIME_DIRECTORY = 'ZenzRuntime'
_ZENZ_MODEL_NAME = 'zenz-v3.2-small-Q5_K_M.gguf'
_ZENZ_BUILD_CONTRACT_NAME = 'BUILD-CONTRACT.txt'
# Must match //mac/installer/zenz_runtime:{apache_license,llama_cpp_license,
# third_party_notices,zenz_model_notice,cpp_httplib_license,
# nlohmann_json_license}.
_ZENZ_RUNTIME_LICENSE_NAMES = (
    'Apache-2.0.txt',
    'THIRD_PARTY_NOTICES.md',
    'cpp-httplib-MIT.txt',
    'llama.cpp-MIT.txt',
    'nlohmann-json-MIT.txt',
    'zenz-v3.2-small-gguf.txt',
)


def ParseArguments() -> argparse.Namespace:
  """Parses command line options."""
  parser = argparse.ArgumentParser()
  parser.add_argument('--input')
  parser.add_argument('--output')
  parser.add_argument('--productbuild', action='store_true')
  parser.add_argument('--noqt', action='store_true')
  parser.add_argument('--oss', action='store_true')
  parser.add_argument('--channel', default='dev')
  parser.add_argument('--work_dir')
  # '-' means pseudo identity.
  # https://github.com/bazelbuild/rules_apple/blob/3.5.1/apple/internal/codesigning_support.bzl#L42
  # https://developer.apple.com/documentation/security/seccodesignatureflags/1397793-adhoc
  parser.add_argument('--codesign_identity', default='-')
  parser.add_argument('--codesign_keychain', default='login.keychain')
  return parser.parse_args()


def RemoveQtFrameworks(app_dir: str, app_name: str) -> None:
  """Change the references to the Qt frameworks."""
  frameworks = ['QtCore', 'QtGui', 'QtPrintSupport', 'QtWidgets']
  host_dir = '@executable_path/../../../ConfigDialog.app/Contents/Frameworks'
  app_file = os.path.join(app_dir, f'Contents/MacOS/{app_name}')

  for framework in frameworks:
    shutil.rmtree(
        os.path.join(app_dir, f'Contents/Frameworks/{framework}.framework/')
    )
    cmd = [
        'install_name_tool',
        '-change',
        f'@rpath/{framework}.framework/Versions/A/{framework}',
        f'{host_dir}/{framework}.framework/{framework}',
        app_file,
    ]
    util.RunOrDie(cmd)


def SymlinkQtFrameworks(app_dir: str) -> None:
  """Create symbolic links of Qt frameworks."""
  frameworks = ['QtCore', 'QtGui', 'QtPrintSupport', 'QtWidgets']
  for framework in frameworks:
    # Creates the following symbolic links.
    #   QtCore.framwwork/QtCore@ -> Versions/A/QtCore
    #   QtCore.framwwork/Resources@ -> Versions/A/Resources
    #   QtCore.framework/Versions/Current@ -> A
    framework_dir = os.path.join(
        app_dir, f'Contents/Frameworks/{framework}.framework/'
    )

    # Restore symlinks. Bazel uses zip without consideration of symlinks.
    # It changes symlink files to normal files. The following logics remove
    # those normal files and make symlinks again.

    # rm {app_dir}/QtCore.framework/Versions/Current
    # ln -s A {app_dir}/QtCore.framework/Versions/Current
    os.remove(framework_dir + 'Versions/Current')
    os.symlink('A', framework_dir + 'Versions/Current')

    # rm {app_dir}/QtCore.framework/QtCore
    # ln -s Versions/Current/QtCore {app_dir}/QtCore.framework/QtCore
    os.remove(framework_dir + framework)
    os.symlink('Versions/Current/' + framework, framework_dir + framework)

    # rm {app_dir}/QtCore.framework/Resources
    # ln -s Versions/Current/Resources {app_dir}/QtCore.framework/Resources
    os.remove(framework_dir + 'Resources')
    os.symlink('Versions/Current/Resources', framework_dir + 'Resources')


def TweakQtApps(top_dir: str, oss: bool) -> None:
  """Tweak the resource files for the Qt applications."""
  name = 'MozkeyIbG' if oss else 'GoogleJapaneseInput'
  sub_qt_apps = [
      'AboutDialog',
      'DictionaryTool',
      'ErrorMessageDialog',
      f'{name}Prelauncher',
      'WordRegisterDialog',
  ]
  for app in sub_qt_apps:
    app_dir = os.path.join(top_dir, f'{name}.app/Contents/Resources/{app}.app')
    RemoveQtFrameworks(app_dir, app)

    # Remove the Frameworks directory, if it's empty.
    framework_dir = os.path.join(app_dir, 'Contents/Frameworks')
    if not any(os.scandir(framework_dir)):
      os.rmdir(framework_dir)

  qt_app = f'{top_dir}/{name}.app/Contents/Resources/ConfigDialog.app'
  SymlinkQtFrameworks(qt_app)


def TweakForProductbuild(
    top_dir: str, tweak_qt: bool, oss: bool, channel: str
) -> None:
  """Tweak file paths for the productbuild command."""
  orig_dir = os.getcwd()
  os.chdir(top_dir)

  if oss:
    is_dev_channel = False
    name = 'MozkeyIbG'
    folder = 'MozkeyIbG'
  else:
    is_dev_channel = channel == 'dev'
    name = 'GoogleJapaneseInput'
    folder = 'GoogleJapaneseInput.localized'

  renames = [
      (f'Uninstall{name}.app', f'root/Applications/{folder}/'),
      (f'{name}.app', 'root/Library/Input Methods/'),
      ('LaunchAgents', 'root/Library/'),
      ('ActivatePane.bundle', 'Plugins/'),
      ('InstallerSections.plist', 'Plugins/'),
      ('postflight.sh', 'scripts/postinstall'),
      ('preflight.sh', 'scripts/preinstall'),
      (
          'Resources/en.lproj/Localizable.strings',
          f'root/Applications/{folder}/.localized/en.strings',
      ),
      (
          'Resources/ja.lproj/Localizable.strings',
          f'root/Applications/{folder}/.localized/ja.strings',
      ),
  ]

  # For the dev channel, add the dev confirm section to the installer.
  if is_dev_channel:
    renames += [('DevConfirmPane.bundle', 'Plugins/')]
  else:
    shutil.rmtree('DevConfirmPane.bundle')
    # Remove the dev confirm section from InstallerSections.plist
    contents = []
    with open('InstallerSections.plist', 'r') as f:
      for line in f:
        if 'DevConfirmPane' in line:
          continue
        contents.append(line)
    with open('InstallerSections.plist', 'w') as f:
      f.write(''.join(contents))

  for src, dst in renames:
    if dst.endswith('/'):
      dst = os.path.join(dst, src)
    os.renames(src, dst)
  os.chmod('scripts/postinstall', 0o755)
  os.chmod('scripts/preinstall', 0o755)

  if tweak_qt:
    resources_dir = f'/Library/Input Methods/{name}.app/Contents/Resources/'
    # /Applications/MozkeyIbG/ConfigDialog.app is a symlink to
    # /Library/Input Method/MozkeyIbG.app/Contents/Resources/ConfigDialog.app
    os.symlink(
        resources_dir + 'ConfigDialog.app/',
        f'root/Applications/{folder}/ConfigDialog.app',
    )
    os.symlink(
        resources_dir + 'DictionaryTool.app/',
        f'root/Applications/{folder}/DictionaryTool.app',
    )

  os.chdir(orig_dir)


def NormalizeAndValidateZenzRuntime(top_dir: str, oss: bool) -> None:
  """Validates and normalizes the canonical packaged Zenz runtime.

  The runtime follows upstream Mozkey's macOS layout: it lives in
  <app>/Contents/Resources/ZenzRuntime, which is where
  session/zenz_client_factory.cc launches mozc_zenz_scorer from.  A package
  built without --define=macos_zenz_runtime=1 lacks llama-server, the model
  and BUILD-CONTRACT.txt and is rejected here instead of shipping an input
  method whose Zenz runtime can never start.
  """
  name = 'MozkeyIbG' if oss else 'GoogleJapaneseInput'
  relative_resources = os.path.join(
      'root',
      'Library',
      'Input Methods',
      f'{name}.app',
      'Contents',
      'Resources',
      _ZENZ_RUNTIME_DIRECTORY,
  )
  resources = os.path.join(top_dir, relative_resources)

  # Reject symlinked or missing directory components so the canonical paths
  # cannot escape the extracted installer tree.
  current = top_dir
  directories = [current]
  for component in relative_resources.split(os.sep):
    current = os.path.join(current, component)
    directories.append(current)
  directories.extend(
      (os.path.join(resources, 'models'), os.path.join(resources, 'licenses'))
  )
  for directory in directories:
    try:
      info = os.lstat(directory)
    except OSError as error:
      raise ValueError('invalid Zenz runtime directory') from error
    if not stat.S_ISDIR(info.st_mode):
      raise ValueError('invalid Zenz runtime directory')

  expected_files = [
      (os.path.join(resources, 'llama-server'), 0o755),
      (os.path.join(resources, 'mozc_zenz_scorer'), 0o755),
      (os.path.join(resources, 'mozc_zenz_guard'), 0o755),
      (os.path.join(resources, 'models', _ZENZ_MODEL_NAME), 0o644),
      (os.path.join(resources, _ZENZ_BUILD_CONTRACT_NAME), 0o644),
  ]
  expected_files.extend(
      (os.path.join(resources, 'licenses', name), 0o644)
      for name in _ZENZ_RUNTIME_LICENSE_NAMES
  )

  # Open every file without following its final component, validate the whole
  # set, and only then mutate modes. This keeps a failed validation from
  # producing a partially normalized package tree.
  opened_files = []
  try:
    for path, expected_mode in expected_files:
      try:
        descriptor = os.open(
            path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0)
        )
      except OSError as error:
        raise ValueError('invalid Zenz runtime file') from error
      opened_files.append((descriptor, expected_mode))
      info = os.fstat(descriptor)
      if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
        raise ValueError('invalid Zenz runtime file')

    for descriptor, expected_mode in opened_files:
      os.fchmod(descriptor, expected_mode)
      if stat.S_IMODE(os.fstat(descriptor).st_mode) != expected_mode:
        raise ValueError('invalid Zenz runtime file mode')
  finally:
    for descriptor, _ in opened_files:
      os.close(descriptor)


def Codesign(
    top_dir: str, identity: str, keychain: str = 'login.keychain'
) -> None:
  """Codesign the installer files."""
  # remove existing _CodeSignature before overwriting the codesigns.
  dir_name = '_CodeSignature'
  for cur_dir, dirs, _ in os.walk(top_dir):  # symbolic links are not followed.
    if dir_name in dirs:
      shutil.rmtree(os.path.join(cur_dir, dir_name))
      dirs.remove(dir_name)  # skip walking the removed directory.

  if not keychain:
    raise ValueError('codesign keychain must not be empty')
  args = ['--force', '--sign', identity, '--keychain', keychain]

  # Hardened runtime and a trusted timestamp are required for notarization.
  # On the other hand, do not add the option for the pseudo identity ('-').
  # https://github.com/google/mozc/issues/1412
  if identity != '-':
    args.extend(('--timestamp', '--options=runtime'))

  # Standalone Mach-O files nested under app Resources must be signed before
  # their enclosing app. --deep is verification-oriented and is not a reliable
  # substitute for signing every nested executable explicitly.
  nested_executables = []
  for cur_dir, dirs, files in os.walk(top_dir):
    dirs.sort()
    for file_name in sorted(files):
      if (
          file_name not in _NESTED_EXECUTABLE_NAMES
          and not file_name.endswith('.dylib')
      ):
        continue
      path = os.path.join(cur_dir, file_name)
      if os.path.islink(path):
        raise ValueError(f'refusing to codesign symlink: {path}')
      if not file_name.endswith('.dylib') and not os.access(path, os.X_OK):
        raise ValueError(f'nested executable is not executable: {path}')
      nested_executables.append(path)

  for path in nested_executables:
    codesign = ['/usr/bin/codesign', *args, path]
    util.RunOrDie(codesign)

  # Upstream Mozkey signs the ZenzRuntime helpers (llama-server,
  # mozc_zenz_scorer, mozc_zenz_guard) in a dedicated pass.  Here they are
  # already covered by _NESTED_EXECUTABLE_NAMES above, which additionally
  # refuses symlinks and non-executable helpers, so they are not signed twice.

  # codesign apps
  # Walk the directory from the bottom to the top. This is necessary because
  # the sub apps should be signed before the main app.
  # Note, os.walk does not folow symbolic links.
  for cur_dir, dirs, _ in os.walk(top_dir, topdown=False):
    for dir_name in dirs:
      path = os.path.join(cur_dir, dir_name)
      ext = os.path.splitext(dir_name)[1]
      is_app = ext in ['.app', '.bundle', '.framework']
      if is_app and not os.path.islink(path):
        codesign = ['/usr/bin/codesign', *args, path]
        util.RunOrDie(codesign)


def TweakInstallerFiles(args: argparse.Namespace, work_dir: str) -> None:
  """Tweak the zip file of installer files to optimize the structure."""
  # Remove top_dir if it already exists.
  top_dir = os.path.join(work_dir, 'installer')
  if os.path.exists(top_dir):
    shutil.rmtree(top_dir)

  # The zip file should contain the 'installer' directory.
  util.RunOrDie(['/usr/bin/ditto', '-x', '-k', args.input, work_dir])

  tweak_qt = not args.noqt

  if tweak_qt:
    TweakQtApps(top_dir, args.oss)

  if args.productbuild:
    TweakForProductbuild(top_dir, tweak_qt, args.oss, args.channel)
    NormalizeAndValidateZenzRuntime(top_dir, args.oss)
    Codesign(
        top_dir,
        args.codesign_identity,
        getattr(args, 'codesign_keychain', 'login.keychain'),
    )

  # Create a zip file with the zip command.
  # It's not easy to contain symlinks with shutil.make_archive.
  if os.path.exists(args.output):
    os.remove(args.output)
  orig_dir = os.getcwd()
  os.chdir(work_dir)
  cmd = ['zip', '-q', '-ry', os.path.join(orig_dir, args.output), 'installer']
  util.RunOrDie(cmd)
  os.chdir(orig_dir)


def main():
  args = ParseArguments()

  if args.work_dir:
    TweakInstallerFiles(args, args.work_dir)
  else:
    with tempfile.TemporaryDirectory() as work_dir:
      TweakInstallerFiles(args, work_dir)


if __name__ == '__main__':
  main()
