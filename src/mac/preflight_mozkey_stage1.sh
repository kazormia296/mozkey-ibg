#!/bin/sh
# Mozkey IbG default (non-release) OSS package: build and inspect ONLY.
# Installation is blocked unless the package was built with the explicit
# release flag (--define=MOZKEY_INSTALLABLE=1), which selects the audited
# release preflight instead of this gate.
echo "This Mozkey IbG PKG is NOT INSTALLABLE: built without MOZKEY_INSTALLABLE=1." >&2
exit 1
