#!/usr/bin/env bash
# Replays the Linux CI (.github/workflows/linux.yml) in an Ubuntu 24.04
# container with the runner's Vulkan loader (1.3.275) and lavapipe, so a
# change can be checked before pushing:
#   tools/ci/run.sh            # the "build" job (tests, f2cec5f invariant, PSNR gates)
#   tools/ci/run.sh asan|tsan  # one "sanitizers" job
#   BDEX_CI_CPUS=4 tools/ci/run.sh ...  # with a GitHub runner's CPU count
# Needs Docker. Works on a copy of the working tree (tracked + untracked files).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
job="${1:-build}"
work="$(mktemp -d)"
trap 'docker run --rm -v "$work:/work" bdex-ci:24.04 rm -rf /work/src /work/baseline >/dev/null 2>&1; rm -rf "$work"' EXIT
docker build -q -t bdex-ci:24.04 "$ROOT/tools/ci" >/dev/null
mkdir -p "$work/src" "$work/baseline"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build' | tar -cf - -T - | tar -xf - -C "$work/src")
(cd "$ROOT" && git archive f2cec5f29d90775a3d1a847620058ffaccca19af | tar -xf - -C "$work/baseline")
cat > "$work/job.sh" <<'JOB'
set -uo pipefail
cd /work/src
export VK_ICD_FILENAMES="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)" LIBGL_ALWAYS_SOFTWARE=1
if [ "$1" = build ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j"$(nproc)" >/dev/null || exit 1
  cmake -S /work/baseline -B /work/baseline/build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build /work/baseline/build -j"$(nproc)" >/dev/null || exit 1
  BDEX_REQUIRE_DISPLAY=1 xvfb-run -a ctest --test-dir build --output-on-failure --repeat until-pass:2 || exit 1
  BDEX_INVARIANT_SEQUENTIAL=1 xvfb-run -a -s '-screen 0 1280x800x24' \
    python3 tests/check_matcher_invariant.py build /work/baseline/build || exit 1
  for a in "" "--speed 2"; do
    echo "PSNR ${a:-normal}: $(xvfb-run -a env BDEX_EVAL_ARGS="$a" tools/run_eval.sh build 40 | grep '^  mean')"
  done
else
  if [ "$1" = asan ]; then
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
    export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
  else
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
  fi
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" >/dev/null
  cmake --build build -j"$(nproc)" >/dev/null || exit 1
  BDEX_REQUIRE_DISPLAY=1 xvfb-run -a -s '-screen 0 1280x800x24' \
    ctest --test-dir build -R "unit|offline|integration|acquire" --output-on-failure
fi
JOB
# BDEX_CI_CPUS=4 caps the container at a GitHub runner's 4 vCPUs: lavapipe
# scales with cores, so an uncapped replay can pass timeouts CI then misses.
docker run --rm ${BDEX_CI_CPUS:+--cpus "$BDEX_CI_CPUS"} --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$work:/work" bdex-ci:24.04 bash /work/job.sh "$job"
