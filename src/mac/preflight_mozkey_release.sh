#!/bin/sh
# Mozkey IbG OSS installable preflight. Selected only by explicit release build
# flag. Every path and identifier below belongs to Mozkey IbG; an upstream Mozkey
# installed side by side is never inspected, gated on, or modified.
# No removal of dictionaries, input sources or preferences.  The only bundle
# ever removed is a verified legacy-layout Mozkey IbG (v0.10.x or earlier), see
# the legacy migration below.
set -eu
PATH='/usr/bin:/bin:/usr/sbin:/sbin'
export PATH

_mozkey_target='@@@MOZKEY_INSTALL_VERSION@@@'
_mozkey_pkg_id='io.github.kazormia296.mozkey-ibg.pkg.JapaneseInput'
_mozkey_bundle_id='io.github.kazormia296.mozkey-ibg.inputmethod.Japanese'
_mozkey_app='/Library/Input Methods/MozkeyIbG.app'
_mozkey_uninstaller='/Applications/MozkeyIbG/UninstallMozkeyIbG.app'

_mozkey_fail() {
  echo "Mozkey IbG preinstall: BLOCKED: $*" >&2
  exit 1
}

_mozkey_valid_pkg_version() {
  printf '%s\n' "$1" | /usr/bin/awk -F. '
    NF != 3 { exit 1 }
    {
      for (i = 1; i <= 3; i++) {
        if ($i !~ /^[0-9]+$/ || length($i) > 3) exit 1
        if (length($i) > 1 && substr($i, 1, 1) == "0") exit 1
      }
      if (($1 + 0) < 100 || ($1 + 0) > 199 || ($2 + 0) > 99 || ($3 + 0) > 99) exit 1
    }'
}

_mozkey_not_newer() {
  /usr/bin/awk -v installed="$1" -v target="$2" 'BEGIN {
    if (split(installed, a, /[.]/) != 3 || split(target, b, /[.]/) != 3) exit 1
    for (i = 1; i <= 3; i++) {
      if (a[i] + 0 > b[i] + 0) exit 1
      if (a[i] + 0 < b[i] + 0) exit 0
    }
    exit 0
  }'
}

_mozkey_read_plist() {
  /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null
}

# Distribution declares rootVolumeOnly; ensure package scripts cannot target a
# different volume while inspecting/modifying the running system's input method.
[ "${3:-}" = / ] || _mozkey_fail 'installation target must be the running system root'
_mozkey_valid_pkg_version "$_mozkey_target" || _mozkey_fail 'invalid embedded target version'

# Reject unexpected filesystem objects and symlink redirection, before even a
# read of the installed app's plist as the root-running installer.
for _mozkey_parent in '/Library' '/Library/Input Methods' '/Library/LaunchAgents' '/Applications' '/Applications/MozkeyIbG'; do
  if [ -L "$_mozkey_parent" ]; then
    _mozkey_fail "unexpected symlink: $_mozkey_parent"
  fi
  if [ -e "$_mozkey_parent" ] && [ ! -d "$_mozkey_parent" ]; then
    _mozkey_fail "unexpected non-directory: $_mozkey_parent"
  fi
done
for _mozkey_path in "$_mozkey_app" "$_mozkey_uninstaller"; do
  if [ -L "$_mozkey_path" ]; then
    _mozkey_fail "unexpected app symlink: $_mozkey_path"
  fi
  if [ -e "$_mozkey_path" ] && [ ! -d "$_mozkey_path" ]; then
    _mozkey_fail "unexpected app path type: $_mozkey_path"
  fi
done

# A receipt is package metadata, NOT proof that its bundle was actually updated.
# PackageKit can advance the receipt while skipping the app bundle.
#
# Mozkey IbG packages up to v0.10.4 were built without an explicit package
# version, so their receipt version is exactly "0", and their bundles carried
# the Mozc 3.33.x build number.  Both are recognised as the legacy format.
_mozkey_receipt=''
if _mozkey_receipt_info=$(/usr/sbin/pkgutil --pkg-info "$_mozkey_pkg_id" 2>/dev/null); then
  _mozkey_receipt=$(printf '%s\n' "$_mozkey_receipt_info" | /usr/bin/awk '
    /^version: / { n++; v=$2 }
    END { if (n == 1) print v }
  ')
  [ -n "$_mozkey_receipt" ] || _mozkey_fail 'unparseable existing package receipt'
else
  _mozkey_known_receipts=$(/usr/sbin/pkgutil --pkgs 2>/dev/null) || _mozkey_fail 'cannot enumerate package receipts'
  if printf '%s\n' "$_mozkey_known_receipts" | /usr/bin/grep -Fqx "$_mozkey_pkg_id"; then
    _mozkey_fail 'package receipt exists but cannot be read'
  fi
fi

_mozkey_legacy_receipt=no
if [ "$_mozkey_receipt" = 0 ]; then
  _mozkey_legacy_receipt=yes
elif [ -n "$_mozkey_receipt" ]; then
  _mozkey_valid_pkg_version "$_mozkey_receipt" || _mozkey_fail "unknown receipt version: $_mozkey_receipt"
  _mozkey_not_newer "$_mozkey_receipt" "$_mozkey_target" || \
    _mozkey_fail "downgrade refused (receipt $_mozkey_receipt > package $_mozkey_target)"
fi

if [ ! -d "$_mozkey_app" ]; then
  # The official Mozkey IbG uninstaller removes application files but historically
  # leaves the package receipt.  A valid receipt alone must not make a fresh
  # installation impossible.  Fail closed if any installed payload remains.
  [ ! -e "$_mozkey_uninstaller" ] || _mozkey_fail 'uninstaller exists without the main app'
  [ ! -e '/Applications/MozkeyIbG' ] || _mozkey_fail 'applications directory remains without the main app'
  for _mozkey_agent in \
    '/Library/LaunchAgents/io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.Converter.plist' \
    '/Library/LaunchAgents/io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.Renderer.plist'; do
    if [ -e "$_mozkey_agent" ] || [ -L "$_mozkey_agent" ]; then
      _mozkey_fail "launch agent remains without the main app: $_mozkey_agent"
    fi
  done
  if [ -n "$_mozkey_receipt" ]; then
    # Only recover the exact root-volume receipt for our own package.  The
    # version/downgrade policy above still applies.  Do not call --forget:
    # Installer will update the receipt after successful installation.
    printf '%s\n' "$_mozkey_receipt_info" | /usr/bin/awk '
      /^package-id: / { ids++; if ($2 != "io.github.kazormia296.mozkey-ibg.pkg.JapaneseInput" || NF != 2) bad=1 }
      /^volume: / { volumes++; if ($2 != "/" || NF != 2) bad=1 }
      /^location: / { locations++; if ($2 != "/" || NF != 2) bad=1 }
      END { if (ids != 1 || volumes != 1 || locations != 1 || bad) exit 1 }
    ' || _mozkey_fail 'unexpected orphan receipt identity, volume or location'
    echo "Mozkey IbG preinstall: ALLOW installation over verified orphan receipt $_mozkey_receipt -> $_mozkey_target"
  else
    echo "Mozkey IbG preinstall: ALLOW clean installation of $_mozkey_target"
  fi
  exit 0
fi

_mozkey_plist="$_mozkey_app/Contents/Info.plist"
[ -f "$_mozkey_plist" ] && [ ! -L "$_mozkey_plist" ] || _mozkey_fail 'main bundle Info.plist missing or a symlink'
_mozkey_current_id=$(_mozkey_read_plist "$_mozkey_plist" CFBundleIdentifier) || _mozkey_fail 'cannot read bundle ID'
[ "$_mozkey_current_id" = "$_mozkey_bundle_id" ] || _mozkey_fail "unexpected bundle identifier: $_mozkey_current_id"
_mozkey_current_executable=$(_mozkey_read_plist "$_mozkey_plist" CFBundleExecutable) || _mozkey_fail 'cannot read bundle executable'
[ "$_mozkey_current_executable" = 'MozkeyIbG' ] || _mozkey_fail 'unexpected bundle executable name'
_mozkey_current_short=$(_mozkey_read_plist "$_mozkey_plist" CFBundleShortVersionString) || _mozkey_fail 'cannot read current short version'
_mozkey_current_build=$(_mozkey_read_plist "$_mozkey_plist" CFBundleVersion) || _mozkey_fail 'cannot read current build version'

if [ -d "$_mozkey_uninstaller" ]; then
  _mozkey_uninstaller_plist="$_mozkey_uninstaller/Contents/Info.plist"
  [ -f "$_mozkey_uninstaller_plist" ] && [ ! -L "$_mozkey_uninstaller_plist" ] || \
    _mozkey_fail 'uninstaller Info.plist missing or a symlink'
  _mozkey_uninstaller_id=$(_mozkey_read_plist "$_mozkey_uninstaller_plist" CFBundleIdentifier) || \
    _mozkey_fail 'cannot read uninstaller bundle ID'
  [ "$_mozkey_uninstaller_id" = 'io.github.kazormia296.mozkey-ibg.inputmethod.Japanese.UninstallMozkeyIbG' ] || \
    _mozkey_fail 'unexpected uninstaller bundle ID'
  _mozkey_uninstaller_executable=$(_mozkey_read_plist "$_mozkey_uninstaller_plist" CFBundleExecutable) || \
    _mozkey_fail 'cannot read uninstaller executable'
  [ "$_mozkey_uninstaller_executable" = 'UninstallMozkeyIbG' ] || _mozkey_fail 'unexpected uninstaller executable name'
fi

# Mozkey IbG bundles up to v0.10.4 carried the Mozc 3.33.x build number
# (CFBundleShortVersionString 3.33.BUILD, CFBundleVersion 3.33.BUILD[.REVISION]).
# The bundle identifier was verified above, so such a bundle is an older Mozkey
# IbG and never an upstream product.  Those packages left receipt version "0";
# a development copy may have no receipt at all.  Do not accept any other
# pre-100 version as if it were older.
if printf '%s\n' "$_mozkey_current_short" | /usr/bin/grep -Eqx '3[.]33[.][0-9]{4}' && \
   { [ "$_mozkey_current_build" = "$_mozkey_current_short" ] || \
     printf '%s\n' "$_mozkey_current_build" | \
       /usr/bin/grep -Eqx "$(printf '%s' "$_mozkey_current_short" | /usr/bin/sed 's/[.]/[.]/g')[.][0-9]{1,3}"; }; then
  # The legacy bundle kept its Zenz runtime inside the nested Converter.app.
  # This package overwrites bundles in place, so those files would survive as
  # unsealed resources and fail the postinstall signature verification.  The
  # legacy packages replaced these same two paths on every install.  Both were
  # verified above: not symlinks, no symlinked parent, Mozkey IbG bundle ID.
  /bin/rm -Rf "$_mozkey_app" || _mozkey_fail 'cannot remove the legacy Mozkey IbG bundle'
  /bin/rm -Rf '/Applications/MozkeyIbG' || _mozkey_fail 'cannot remove the legacy Mozkey IbG applications directory'
  echo "Mozkey IbG preinstall: ALLOW legacy Mozkey IbG $_mozkey_current_build migration to $_mozkey_target"
  exit 0
fi

_mozkey_valid_pkg_version "$_mozkey_current_build" || \
  _mozkey_fail "unsupported existing app version: $_mozkey_current_build"
_mozkey_derived_short=$(printf '%s\n' "$_mozkey_current_build" | /usr/bin/awk -F. '{ printf "%d.%d.%d", ($1 - 100), $2, $3 }')
[ "$_mozkey_current_short" = "$_mozkey_derived_short" ] || \
  _mozkey_fail 'installed application short/build versions disagree'
[ -n "$_mozkey_receipt" ] && [ "$_mozkey_legacy_receipt" = no ] || \
  _mozkey_fail 'Mozkey IbG app exists without a current-format package receipt'
_mozkey_not_newer "$_mozkey_current_build" "$_mozkey_target" || \
  _mozkey_fail "downgrade refused (app $_mozkey_current_build > package $_mozkey_target)"


echo "Mozkey IbG preinstall: ALLOW same-path version $_mozkey_current_build -> $_mozkey_target"
exit 0
