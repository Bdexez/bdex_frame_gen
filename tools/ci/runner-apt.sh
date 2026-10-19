#!/bin/bash
# apt-get update + install on the GitHub runner itself, time-bounded.
# Usage: tools/ci/runner-apt.sh PACKAGE...
# The runner's sources go through a mirror list (/etc/apt/apt-mirrors.txt):
# azure.archive.ubuntu.com first, archive.ubuntu.com after. When the Azure
# mirror stalls, apt-get update does not fail: it hung for 4 hours on
# 2026-10-08 ("Ign: http://azure.archive.ubuntu.com ... Packages") until
# GitHub's 6-hour job limit. Here each attempt is bounded (short network
# timeouts, then `timeout`), and after a failed attempt the Azure mirror is
# dropped from the list so the next one goes to archive.ubuntu.com.
set -uo pipefail
opts=(-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::Retries=2)
drop_azure() {
  local f
  for f in /etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d/*; do
    [ -f "$f" ] && grep -q azure.archive.ubuntu.com "$f" || continue
    if [ "$f" = /etc/apt/apt-mirrors.txt ]; then
      sudo sed -i '/azure\.archive\.ubuntu\.com/d' "$f"
    else
      sudo sed -i 's#http://azure\.archive\.ubuntu\.com#http://archive.ubuntu.com#g' "$f"
    fi
    echo "runner-apt: Azure mirror removed from $f"
  done
}
for i in 1 2 3; do
  if sudo timeout 300 apt-get "${opts[@]}" update &&
     sudo timeout 600 apt-get "${opts[@]}" install -y --no-install-recommends "$@"; then
    exit 0
  fi
  echo "runner-apt: attempt $i failed (or timed out)" >&2
  drop_azure
  sleep $((i * 10))
done
echo "runner-apt: apt-get failed 3 times" >&2
exit 1
