#!/usr/bin/env bash
# Copyright 2026 The Mozkey Authors

set -euo pipefail

readonly SNAPSHOT_DATE="2026/07/17"
cat > /etc/pacman.d/mirrorlist <<EOF
Server = https://archive.archlinux.org/repos/${SNAPSHOT_DATE}/\$repo/os/\$arch
EOF

# archive.archlinux.org can temporarily throttle a GitHub-hosted runner below
# pacman's low-speed threshold while serving a fixed snapshot.
pacman -Syy --noconfirm --disable-download-timeout
