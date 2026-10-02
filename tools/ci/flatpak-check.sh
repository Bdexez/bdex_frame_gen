#!/usr/bin/env bash
# Builds the Flatpak Vulkan layer extension (packaging/flatpak) from this
# checkout in a container, installs it there, and checks that the Freedesktop
# runtime's own Vulkan loader loads it, 64 and 32-bit. Nothing touches the
# host's Flatpak installation (the SDK is cached in a Docker volume).
#   tools/ci/flatpak-check.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker volume create bdex-flatpak-cache >/dev/null
exec docker run --rm --privileged -v "$ROOT":/src:ro -v bdex-flatpak-cache:/root/.local/share/flatpak \
  fedora:latest bash /src/tools/ci/flatpak-check-inner.sh
