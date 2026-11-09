#!/usr/bin/env bash
# The Linux CI's tests (.github/workflows/linux.yml, and tools/ci/run.sh in
# Docker), by lane: each lane on its own virtual display (xvfb-run -a), the
# lanes one after the other — all of them, even after a failure, so that a
# run reports everything — and the script fails if one of them did.
#   tools/ci/linux-tests.sh BUILD BASELINE_BUILD [LANE...]
#   lanes: core display mingw (every push, linux.yml), slow validation
#   stress (nightly, compat.yml); default: all but mingw
# BDEX_CORPUS_DIR adds the recorded game sequences to the corpus test.
#
# Not side by side (measured 2026-11-01, the CI replayed on 2 CPUs — a
# private repository's runner): three lanes at once made the offline tests
# 5 to 6 times slower and failed the pacing tests (capture_linux,
# limit_session: the demo fell to 2 fps), which wait on clocks only as long
# as they have a CPU.
#
# Times on 2 CPUs (2026-11-01): build 67 s; core 362 s (determinism 154,
# offline 75, corpus 49, output_reuse 33, invariant 3, PSNR gates 26);
# display 168 s (capture_linux 61, limit_session x2 65, live_config 26);
# mingw 26 s; stress 166 s; validation 552 s — the two lanes under the
# Khronos layers went to the nightly run: alone they were 12 of a push's
# 21 minutes of tests. On GitHub's runner, 1.8 times as long (the first run,
# 2026-11-01: tests 16 min 26, build 3 min 40 with an empty compiler cache):
# determinism and output_reuse (the slow lane, 187 s here) went to the
# nightly run too, to keep the push under 15 minutes (the user's choice).
set -uo pipefail
build=${1:?build directory}
baseline=${2:?f2cec5f build directory}
cd "$(dirname "$0")/../.."
logs="${BDEX_CI_LOGS:-$build/ci-logs}"
mkdir -p "$logs"
display_re='integration|latency_probe|gamescope_limit|capture_linux|live_config|limit_session|record_live|replay_bench|acquire_timeout|multithread_config'
slow_re='^determinism$|^output_reuse$'
# The core lane: every test the others do not run.
others_re="^validation\$|swapchain_stress|$slow_re|$display_re"
start=$(date +%s)
stamp() { echo "[$(( $(date +%s) - start ))s] $*"; }

# GPU work without a display (bdex_offline), the f2cec5f invariant and the
# PSNR gates.
lane_core() {
  xvfb-run -a ctest --test-dir "$build" --output-on-failure --repeat until-pass:2 \
    -E "$others_re" || return 1
  # Default generated frames must match the pre-census build byte for byte.
  # Each run loads only its own build's layer (private XDG_DATA_HOME).
  BDEX_INVARIANT_SEQUENTIAL=1 xvfb-run -a -s '-screen 0 1280x800x24' \
    python3 tests/check_matcher_invariant.py "$build" "$baseline" || return 1
  # Quality regression gate: PSNR of the generated frames against the 60 fps
  # ground truth, normal and fast-motion scenes (lavapipe tracks the GPU
  # within ~0.04 dB; thresholds just under it).
  local normal fast report mult
  normal="$(xvfb-run -a tools/run_eval.sh "$build" 40 | awk '/^  mean/{print $2; exit}')"
  fast="$(xvfb-run -a env BDEX_EVAL_ARGS='--speed 2' tools/run_eval.sh "$build" 40 | awk '/^  mean/{print $2; exit}')"
  echo "PSNR normal: ${normal} dB, fast: ${fast} dB"
  python3 - "$normal" "$fast" <<'EOF' || return 1
import sys
try:
    normal, fast = float(sys.argv[1]), float(sys.argv[2])
except ValueError:
    print("quality eval produced no mean PSNR"); sys.exit(1)
failures = []
if normal < 28.0: failures.append(f"normal {normal:.2f} < 28.0 dB")
if fast < 23.0:   failures.append(f"fast {fast:.2f} < 23.0 dB")
if failures:
    print("quality regression:", "; ".join(failures)); sys.exit(1)
EOF
  # x3/x4 score every generated position (local reference on llvmpipe:
  # 31.81/31.98 dB at 800x600); same 28 dB floor, shorter runs.
  for mult in 3 4; do
    report="$(BDEX_FG_MULTIPLIER=$mult xvfb-run -a tools/run_eval.sh "$build" 12)"
    echo "$report"
    awk '/^  mean/ { seen=1; if ($2 < 28) bad=1 } END { exit !seen || bad }' <<<"$report" || return 1
  done
}

# The demo through the layer on a display (the compositor test included:
# capture_linux starts openbox + picom on its display).
lane_display() {
  BDEX_REQUIRE_DISPLAY=1 BDEX_TEST_COMPOSITOR=1 xvfb-run -a \
    ctest --test-dir "$build" --output-on-failure --repeat until-pass:2 -R "$display_re"
}

# Khronos validation (synchronization included) over every option.
lane_validation() {
  BDEX_REQUIRE_DISPLAY=1 xvfb-run -a \
    ctest --test-dir "$build" --output-on-failure --repeat until-pass:2 -R '^validation$'
}

# Every option run twice, byte-identical (determinism), and outputs shared
# at equal display moments against full synthesis (output_reuse).
lane_slow() {
  xvfb-run -a ctest --test-dir "$build" --output-on-failure --repeat until-pass:2 -R "$slow_re"
}

# The swapchain stress run (resizes, recreations, fullscreen) under the
# Khronos layers.
lane_stress() {
  BDEX_REQUIRE_DISPLAY=1 xvfb-run -a \
    ctest --test-dir "$build" --output-on-failure --repeat until-pass:2 -R 'swapchain_stress'
}

lane_mingw() {
  BDEX_MINGW_ARCHS=i686 BDEX_MINGW_GEN="$build" tools/ci/mingw/check.sh
}

shift 2
lanes=("$@")
[ ${#lanes[@]} -gt 0 ] || lanes=(core display slow stress validation)
summary="${GITHUB_STEP_SUMMARY:-/dev/null}"
{ echo "### Tests on $(nproc) CPUs"; echo; echo "| lane | exit | seconds |"; echo "|---|---|---|"; } >> "$summary"
status=0
for l in "${lanes[@]}"; do
  declare -F "lane_$l" >/dev/null || { echo "unknown lane: $l"; status=1; continue; }
  t0=$(date +%s)
  "lane_$l" > "$logs/$l.log" 2>&1
  rc=$?
  secs=$(( $(date +%s) - t0 ))
  [ -n "${GITHUB_ACTIONS:-}" ] && echo "::group::$l (exit $rc, ${secs}s)"
  # A passing lane's output is long (the demo's own log): one line a test
  # with its time, and the gates' figures (the full log stays in $logs).
  if [ "$rc" = 0 ]; then
    grep -E "Test +#|tests passed|Total Test time|PSNR|^  mean|files checked|identical|differ|resolved" "$logs/$l.log"
  else
    cat "$logs/$l.log"
  fi
  [ -n "${GITHUB_ACTIONS:-}" ] && echo "::endgroup::"
  stamp "$l: exit $rc after ${secs}s"
  echo "| $l | $rc | $secs |" >> "$summary"
  [ "$rc" = 0 ] || { status=1; [ -n "${GITHUB_ACTIONS:-}" ] && echo "::error::lane $l failed (exit $rc)"; }
done
exit $status
