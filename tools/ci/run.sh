#!/usr/bin/env bash
# Replays the Linux CI (.github/workflows/linux.yml) in an Ubuntu 24.04
# container with the runner's Vulkan loader (1.3.275) and lavapipe, so a
# change can be checked before pushing:
#   tools/ci/run.sh            # the "test" job (tools/ci/linux-tests.sh: tests, f2cec5f invariant, PSNR gates)
#   tools/ci/run.sh asan|tsan  # one "sanitizers" job (compat.yml, nightly)
#   tools/ci/run.sh mingw      # the Windows C++ with MinGW, x64 and 32-bit x86 (catches Win32-only errors)
#   BDEX_CI_CPUS=2 tools/ci/run.sh ...  # with a GitHub runner's CPU count (private repository: 2)
# Needs Docker. Works on a copy of the working tree (tracked + untracked files).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
job="${1:-build}"
work="$(mktemp -d)"
trap 'docker run --rm -v "$work:/work" bdex-ci:24.04 rm -rf /work/src /work/baseline >/dev/null 2>&1; rm -rf "$work"' EXIT
if [ "$job" = mingw ]; then
  docker build -q -t bdex-ci-mingw:24.04 "$ROOT/tools/ci/mingw" >/dev/null
  mkdir -p "$work/src"
  (cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build' | tar -cf - -T - | tar -xf - -C "$work/src")
  # (No exec: the EXIT trap must still remove the copy of the sources.)
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$work:/work" bdex-ci-mingw:24.04 \
    bash /work/src/tools/ci/mingw/check.sh
  exit $?
fi
docker build -q -t bdex-ci:24.04 "$ROOT/tools/ci" >/dev/null
mkdir -p "$work/src" "$work/baseline"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build' | tar -cf - -T - | tar -xf - -C "$work/src")
(cd "$ROOT" && git archive f2cec5f29d90775a3d1a847620058ffaccca19af | tar -xf - -C "$work/baseline")
cat > "$work/job.sh" <<'JOB'
set -uo pipefail
cd /work/src
export VK_ICD_FILENAMES="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)" LIBGL_ALWAYS_SOFTWARE=1
if [ "$1" = build ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBDEX_WERROR=ON >/dev/null && cmake --build build -j"$(nproc)" >/dev/null || exit 1
  cmake -S /work/baseline -B /work/baseline/build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build /work/baseline/build -j"$(nproc)" >/dev/null || exit 1
  # The CI job's own test script: its lanes, invariant and PSNR gates.
  tools/ci/linux-tests.sh build /work/baseline/build || exit 1
else
  if [ "$1" = asan ]; then
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
    export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
  else
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=$PWD/tests/tsan.supp
  fi
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" >/dev/null
  cmake --build build -j"$(nproc)" >/dev/null || exit 1
  BDEX_REQUIRE_DISPLAY=1 xvfb-run -a -s '-screen 0 1280x800x24' \
    ctest --test-dir build -R "unit|offline|integration|acquire" --output-on-failure
fi
JOB
# BDEX_CI_CPUS=N gives the container N CPUs (the last ones), as a runner
# has: lavapipe scales with cores, so an uncapped replay can pass timeouts
# CI then misses. A CPU set, not --cpus: under --cpus, nproc still sees
# every CPU, and lavapipe's threads (and make -j) oversubscribe the quota.
cpuset=""
if [ -n "${BDEX_CI_CPUS:-}" ]; then
  n=$(nproc); cpuset="--cpuset-cpus $(( n - BDEX_CI_CPUS ))-$(( n - 1 ))"
fi
docker run --rm $cpuset --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$work:/work" bdex-ci:24.04 bash /work/job.sh "$job"
