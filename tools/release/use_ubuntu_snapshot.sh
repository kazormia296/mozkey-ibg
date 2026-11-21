#!/usr/bin/env bash
# Copyright 2026 The Mozkey Authors

set -euo pipefail

readonly SNAPSHOT_URL="https://snapshot.ubuntu.com/ubuntu/20261120T000000Z/"
readonly SOURCES_FILE="/etc/apt/sources.list.d/mozkey-snapshot.sources"
readonly APT_UPDATE_ATTEMPTS=5
readonly TEMP_SOURCES="$(mktemp)"
trap 'rm -f -- "${TEMP_SOURCES}"' EXIT

cat > "${TEMP_SOURCES}" <<EOF
Types: deb
URIs: ${SNAPSHOT_URL}
Suites: noble noble-updates noble-security
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

# Keep the build input limited to the pinned Ubuntu snapshot.  Runner images
# also carry vendor sources (for example Microsoft and Google) that would
# otherwise make package resolution non-reproducible.
sudo rm -f -- \
  /etc/apt/sources.list \
  /etc/apt/sources.list.d/*.list \
  /etc/apt/sources.list.d/*.sources
sudo install -m 644 "${TEMP_SOURCES}" "${SOURCES_FILE}"

# snapshot.ubuntu.com can transiently return 5xx responses for one index.
# Treat partial indexes as a failure instead of allowing apt to reuse stale
# runner metadata, then retry the complete update.
for ((attempt = 1; attempt <= APT_UPDATE_ATTEMPTS; attempt++)); do
  if sudo apt-get \
    -o Acquire::Check-Valid-Until=false \
    -o Acquire::Retries=3 \
    -o APT::Update::Error-Mode=any \
    update; then
    exit 0
  fi

  if ((attempt < APT_UPDATE_ATTEMPTS)); then
    sleep $((attempt * 5))
  fi
done

echo "Ubuntu snapshot apt index update failed after ${APT_UPDATE_ATTEMPTS} attempts." >&2
exit 1
