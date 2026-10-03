#!/usr/bin/env bash
# Fuzzes the parsers (tests/fuzz/fuzz.cpp: configuration and profiles,
# status files, options_ui.conf, online_tune cache) with libFuzzer, under
# AddressSanitizer and UndefinedBehaviorSanitizer, from the seed corpus.
# A failing input is copied to tests/fuzz/corpus/<target>/ (commit it with
# the fix: every build then replays it, ctest fuzz_<target>).
#   tools/ci/fuzz.sh [seconds per target] [target...]   (default: 60, all)
# Needs Clang with libFuzzer and the sanitizer runtimes (CC/CXX to choose one;
# Ubuntu: clang libclang-rt-18-dev).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
secs="${1:-60}"; shift || true
targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(config status options_ui online_tune)
build="${BDEX_FUZZ_BUILD:-$ROOT/build-fuzz}"
san="-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
cmake -S "$ROOT" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="${CC:-clang}" \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DCMAKE_CXX_FLAGS="$san" -DCMAKE_C_FLAGS="$san" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DBDEX_FUZZ=ON \
  -DBDEX_BUILD_DEMO=OFF -DBDEX_BUILD_OFFLINE=OFF >/dev/null
cmake --build "$build" -j"$(nproc)" --target $(printf 'bdex_fuzz_%s ' "${targets[@]}") >/dev/null
status=0
for t in "${targets[@]}"; do
  work="$(mktemp -d)"
  cp "$ROOT/tests/fuzz/corpus/$t"/* "$work/" 2>/dev/null || true
  # The options file as a dictionary for the configuration: its keys.
  dict=()
  if [ "$t" = config ]; then
    grep -oE '^\| `[a-z_0-9]+`' "$ROOT/README.md" | sed 's/^| `\(.*\)`/"\L\1"/' | sort -u > "$work.dict" || true
    printf '"="\n"["\n"]"\n"*"\n"auto"\n"0.5"\n"-1"\n"1e9"\n' >> "$work.dict"
    dict=(-dict="$work.dict")
  fi
  echo "== $t: ${secs}s"
  # alloc_dealloc_mismatch=0: std::stable_sort's temporary buffer
  # (libstdc++ 16's stl_tempbuf.h built by Clang 22) reads as "operator new
  # vs free" to ASan — the library's code, on every input, not ours.
  if ! (cd "$work" && ASAN_OPTIONS=alloc_dealloc_mismatch=0 UBSAN_OPTIONS=print_stacktrace=1 "$build/tests/bdex_fuzz_$t" "${dict[@]}" -max_total_time="$secs" \
        -max_len=4096 -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 "$work" 2>"$work.log"); then
    status=1
    grep -E "ERROR|SUMMARY|fuzz:|runtime error" "$work.log" | head -20
    for c in "$work"/crash-* "$work"/timeout-* "$work"/oom-* ; do
      [ -f "$c" ] && cp "$c" "$ROOT/tests/fuzz/corpus/$t/" && echo "  failing input: tests/fuzz/corpus/$t/$(basename "$c")"
    done
  fi
  grep -E "stat::number_of_executed_units|cov:" "$work.log" | tail -2
  rm -rf "$work" "$work.log" "$work.dict"
done
exit $status
