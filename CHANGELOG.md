# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added (2026-09-25)

- CI: a `windows-syntax` job in `linux.yml` runs the MinGW x64 / x86
  syntax check on every push (the check caught both Windows breakages of
  the day before they reached MSVC, but only when run by hand).
- **Windows: live status in the capture app's launcher.** `bdex_capture`
  publishes its state like the layer (`kind=capture`, with the captured
  game, fps, latency from WGC to the present call, passes, `online_tune`),
  and the launcher gains an **En cours** frame that reads the status files
  every second (layer games and captures; files of dead processes skipped).
  Parsing and the French summary are shared, unit-tested code
  (`statusParse`, `statusDescribe` in `status.cpp`).
- **Richer diagnostics.** The live status of a swapchain now lists the
  optional passes at work (`passes=`, `FgPass::active`) and `online_tune`'s
  state (`online_tune=`: its decisions, or the option on trial and how many
  are decided); the GTK panel's "En cours" view shows both, in French.
  `bdex-framegen --report` adds the live status of the running games and
  the cached `online_tune` decisions, so a bug report carries what used to
  take a log file (the pass-through reason and `gpu_budget` level were
  already in the status).
- **`online_tune` tries the synthesis options, and again when the content
  changes.** A synthesis probe (`passes/synthesis_probe.{h,cpp}`,
  `flow_compose.comp`, `probe_compare.comp`) runs the engine's own
  synthesis (a linear-output variant) on a frame the game rendered but the
  synthesis did not see — interpolation: the middle of three frames from
  the outer two with composed flows; extrapolation: the next frame — and
  compares it (RMS, weighted like PSNR; mean absolute error missed fixes
  of a few badly wrong pixels) with a naive predictor. Candidates:
  `occlusion_strength`, `pixel_vectors`, `synth_bicubic` (interpolation),
  `extrapolate_splat`, `extrapolate_accel`, `synth_bicubic` (extrapolation);
  `static_guard` is not one, the probe's two-frame gap magnifies its known
  failure. Offline the decisions follow the PSNR (accel kept on a swinging
  camera, +0.94 dB; splat on a pan, +0.82 dB; each dropped where it loses).
  Once decided, the engine keeps watching motion and brightness (every 8th
  frame) and starts the trial again on a lasting change (tested with a
  bright pan followed by a dark swinging scene: `zero_bias_contrast`,
  +3.4 dB there, is re-decided). A strong prediction gain now stands even
  when the flow changes more (tracking an accelerating camera). The
  validation test gains two probe runs (primed cache). Defaults
  byte-identical.
- **`online_tune` rates stability too.** The self-evaluation also sums the
  flow's change from one pair to the next for the same content, relative to
  the motion; an option is kept for a prediction gain that does not
  destabilise the flow (+5 % at most), or for a clearly steadier flow
  (−10 %) that does not hurt the prediction. The prediction error alone
  cannot see flicker: `flow_temporal`, which lowers it, was always dropped;
  on the demo it is now kept (flow change −16 %). Decisions and caches carry
  the stability (older caches still read).
- **`hud_mask` (opt-in): learned static-overlay mask.** An `FgPass`
  (`hud_mask.comp`) counts per render pixel the real frames it stayed
  unchanged while the camera moved; past the threshold (24 advised) every
  synthesis keeps the pixel as in the real frame. The synthesis gained a
  generic "static mask" binding (1×1 zero image when off) that a pass
  provides through `FgContext::setStaticMask`. Learning only under camera
  motion (a quarter of the pixels changing) was measured necessary: with
  any motion counting, the demo's fixed-camera scene lost 0.3 dB in
  extrapolation (still background taken for an overlay). HUD scene +0.91 dB
  interpolated, +0.61 dB extrapolated (where `static_guard` does nothing);
  scenes without overlay unchanged. Default 0 byte-identical.
- **`subpixel_lk` and `synth_bicubic` (opt-in): accurate sub-pixel motion.**
  `subpixel_lk` (an `FgPass`, `flow_lk.comp`) refines each finest vector by
  Lucas-Kanade — Gauss-Newton steps over an 8×8 window, kept only where the
  fit is well posed, stays within a pixel and lowers the SAD; +0.1 ms.
  Measuring it found the real limit of sub-pixel vectors in interpolation:
  the synthesis's bilinear sampling (with it alone, as with `subpixel`,
  −0.8 dB on a sub-pixel pan). `synth_bicubic` samples along the
  trajectories with a clamped 9-tap Catmull-Rom: together +0.37 dB there,
  demo +0.09 dB, neutral elsewhere. In extrapolation +2.1 dB on that pan.
  Lucas-Kanade matches the parabolic fit on single frames and beats it in
  `upscale_temporal` accumulation (weight 0.3: +0.83 vs +0.67 dB, swinging
  camera +0.01 vs −0.16 dB). `subpixel_lk` is an `online_tune` candidate in
  extrapolation. Default 0 byte-identical; in the validation state runs.
- `tools/ci/run.sh mingw`: syntax-compiles what the Windows CI builds with
  MinGW for x64 and 32-bit x86 (i686), in Docker. The 32-bit pass reports
  the Win32-only MSVC errors (Vulkan handle casts) before a push; it
  catches the `test_descriptor_planner` error the Win32 CI found.
- **`upscale_temporal` (opt-in): flow-guided temporal upscaling** for
  `render_scale` < 1. A pass (`upscale_temporal.comp`) upscales the real
  frame into a display-size rgba16f history (ping-pong) and blends in the
  previous one carried along the backward flow, with TAA-style rejection
  (3×3 neighbourhood clamp at mean ± 1.25σ, scene cut, off-picture, and a
  forward/backward round-trip check — without it the wrong flow of
  entering content smeared the border and cost up to 4 dB). The real
  frame is packed from the history, and the generated frames are
  synthesised from the two histories rather than from the small source
  images. Measured at render scale 0.5 with `0.05`: +0.03 to +1.29 dB on
  every sequence, flicker −30 % on the HUD scene; at 0.75 within ±0.1 dB
  (HUD scene −0.13). Heavier accumulation loses (no camera jitter to
  gather, integer flow). Default 0 byte-identical, with or without
  upscaling; covered by the validation state runs.
- `bdex_offline --score-real`: the real frames enter the manifest as the
  engine outputs them, so `eval_sequence.py --offline-args "--render-scale
  0.5 --score-real"` scores the upscaler too.
- **`online_tune` (opt-in): per-game automatic choice of the motion
  options.** A new pass (`self_eval.comp`) rates the motion field on the
  game's own next frame: the previous pair's motion predicts it at
  constant velocity and the error is compared with predicting no motion
  (sampled grid, fixed-point sums per command-buffer slot, read back when
  the slot is reused — no stall). `OnlineTune` (`online_tune.{h,cpp}`,
  unit-tested) tries each candidate in alternating ABBA blocks of 8 flows
  on top of the options already kept, skips frames without motion, and
  keeps an option that lowers the error ratio by ≥ 0.01 on average and in
  3 pairs out of 4. Candidates: `zero_bias_contrast=0.1`,
  `temporal_prediction=1`, `subpixel=1` (extrapolation only — measured
  −0.9 dB interpolated where the measure favours it), `flow_temporal=1`;
  options the user set are not candidates. Decisions are logged and cached
  per game and mode after each one (`platformCacheDir()`,
  `BDEX_FG_CACHE_DIR`), so the next session starts tuned. The demo is
  tuned in 6.6 s at 60 fps; offline the choices match the PSNR (e.g.
  `zero_bias_contrast` +4.75 dB kept on a swinging camera, dropped on the
  neutral HUD scene). Also in the capture app (its command slots feed the
  read-back), the GTK panel and the launcher. Default 0 is byte-identical;
  covered by the validation state and async runs.
- **`flow_temporal` (opt-in): temporal stabilisation of the motion.** A new
  pass after the finest flow (`flow_temporal.comp`) reconsiders each vector
  against the same content's motion one pair earlier (the previous backward
  flow, followed along the new one for the backward direction) and keeps
  the old vector where it is close (≤ 2 px) and its block difference is
  within a tolerance of the new one's — the ambiguous matches that switch
  from pair to pair and shimmer. A first version that blended the two
  vectors lost 0.5 dB on the demo (it lagged genuine motion changes) and
  was replaced by this cost-checked choice. Offline demo replay:
  interpolation +0.13 dB (moving edges +0.25, flicker −5 %), extrapolation
  +0.05 dB (moving edges +0.25), fast motion +0.01, synthetic corpus
  neutral; GPU time within noise. Default 0 is byte-identical; covered by
  the validation state and async runs.
- **`async_compute` (opt-in): the synthesis on a compute-only queue.** Where
  the device has a compute-only queue family with a free queue, the layer
  takes one for the synthesis and keeps copies and presents on its
  presentation queue: resources are shared concurrently between the two
  families, each present waits its own semaphore from the synthesis (or,
  deferred, from the game), each copy signals one the next synthesis waits
  before rewriting what it read, semaphores of skipped or failed presents
  are consumed, and both queues are drained before anything is destroyed or
  rebuilt. Timestamps are only used where every family supports them. The
  validation test gains two async runs (0 messages). Measured on an RTX
  3060 Ti: the driver runs that queue at half speed (flow 1.25 → 2.5 ms)
  and the demo drops from 432 to ~315 fps, so it stays off by default; it
  is meant for GPUs whose compute engines overlap graphics (AMD), to be
  measured there.
- **Live status in the control panel.** Each process where the layer is
  enabled publishes a small status file (`$XDG_RUNTIME_DIR/bdex-framegen/
  <pid>.status`, `%TEMP%\bdex-framegen` on Windows, `BDEX_FG_STATUS_DIR` to
  move or disable it), rewritten atomically at most once a second and
  removed on exit: per swapchain its state (generating, paused,
  pass-through), game and output fps, multiplier and mode, measured refresh
  rate and latency, `gpu_budget` level — and **why** it passes frames
  through (format, image usage, queue, anti-cheat, creation failure…),
  which used to take a log file. The GTK panel gains an "En cours" section
  refreshed every second; stale files of crashed games are cleaned up.
  Disabled processes (every other Vulkan program) write nothing.
- **Measured latency, game present to screen.** With present feedback, each
  real frame's display time is compared with the game's present call: the
  statistics line reports `present-to-screen N ms` (demo on an RTX 3060 Ti:
  interpolation 16.2 ms, extrapolation 9.2 ms — the real-frame hold shows)
  and HUD level 5 (`hud = latency`) adds it in cyan. Where the driver cannot
  report display times the HUD shows `--ms` and nothing else changes. The
  former "real frame delayed" figure only counted CPU submission time.
- **HDR in the Windows capture app.** It captured and presented in BGRA8,
  flattening HDR games to SDR. When the game's monitor is in HDR mode
  (DXGI output colour space PQ/BT.2020; `capture_hdr = auto`, the default)
  and the GPU can import FP16 D3D11 textures and the overlay surface offers
  scRGB, the whole chain runs in scRGB: WGC frame pool and D3D11 ring in
  `R16G16B16A16_FLOAT`, Vulkan import and history in FP16, an
  `EXTENDED_SRGB_LINEAR` overlay swapchain (`VK_EXT_swapchain_colorspace`),
  and the engine on its HDR matching path. Any missing link keeps SDR, as
  before (logged). `capture_hdr = 0` forces SDR, `1` HDR where possible; the
  launcher gains the choice. Not run on hardware (no Windows machine here):
  `vkctx.cpp`, `main.cpp` and `launcher.cpp` were syntax-checked with MinGW,
  `capture.cpp` (C++/WinRT) is left to the Windows CI.
- **Present feedback: the layer measures when frames are really
  displayed.** Where the device supports `VK_KHR_present_id` and
  `VK_KHR_present_wait` (still hidden from the game), the layer enables them
  for itself (device creation retried without them if it fails), tags every
  present of its real swapchain and waits for each on a thread (paused
  while the swapchain is re-created, stopped before it is destroyed).
  `DisplayClock` (unit-tested) learns the refresh grid from the display
  times — the largest period every interval is a multiple of, within a
  tolerance that shrinks with the period — and stays invalid on
  variable-refresh displays or compositors without a grid (the case of the
  development machine, Hyprland, where nothing changes). With a grid: the
  pacing presents each frame 0.3 refresh ahead of the refresh it should
  show on; the per-frame state pass gets each frame's predicted display
  time, so generated frames show the content of their real display moment
  (on by default; content timing still follows `frame_timing`/`dedup`);
  `target_fps = auto` and `fps_limit = auto` use the measured rate; the
  statistics report the refresh rate and frames replaced before display.
  Offline (`bdex_offline --refresh HZ [--refresh-known]` simulates a
  fixed-refresh display): 60 fps game at x2 on 144 Hz +2.1 to +3.1 dB, on
  100 Hz +10 to +11 dB, on 75 Hz +15 dB; byte-identical without a grid.
  The GTK panel shows `auto` as −1 and gains the switch.

### Changed (2026-09-25)

- **The last optional features moved into passes**: `extrapolate_accel`,
  `extrapolate_splat` (a `beforeSynthesis` hook fills `SynthesisInputs`; the
  splat map is bound through `FgContext::setSplatMap`) and the fps HUD (now
  `hud_overlay.comp`, drawn over the finished output through an
  `afterOutput` hook; the synthesis and upscale shaders lost their HUD
  code and push constants). Byte-identical, the HUD included — except that
  with CAS sharpening the HUD is no longer sharpened. `dedup` stays in
  `FrameGen`: it is the pipeline's control flow (flow dispatches cancelled
  indirectly, the state buffer shared with `frame_timing`), not an added
  pass.
- **Optional passes behind one interface** (`fg_pass.h`): an opt-in GPU
  feature is an `FgPass` in `layer/src/passes/` with `create`/`destroy` and
  named pipeline hooks, seeing the engine only through `FgContext`;
  `passes/registry.cpp` lists them in hook order. `flow_temporal`,
  `online_tune` (self-evaluation) and `upscale_temporal` moved there out of
  `FrameGen` (~200 lines and five touch points each). The engine sources
  are one CMake list (`BDEX_ENGINE_SOURCES`) shared by the layer,
  `bdex_offline` and `bdex_capture`. Output byte-identical; the options
  measure as before.
- `framegen.cpp` split by feature: `framegen_tune.cpp` (self-evaluation and
  `online_tune`), `framegen_output.cpp` (the real frame's output stage:
  spatial / temporal upscaling, HUD, dedup replacement, CAS), and
  `framegen_internal.h` (push-constant blocks, state-buffer layout, shared
  helpers, private to these files). The layer, `bdex_offline` and
  `bdex_capture` build all three. No behaviour change.
- **Option registry.** The plain bool / int / float options are declared
  once, in a table in `config.cpp` (key, alias, range, live-reloadable,
  `online_tune` candidate and trial value): `Config::apply`,
  `Config::applyLive` and the `online_tune` candidates are driven by it
  instead of ~50 hand-written branches, a copy list and a separate
  candidate list in `framegen.cpp`. `Config::optionKeys()` and
  `bdex_offline --list-options [--canonical]` list every key. Parsing and
  output unchanged (byte-identical).
- **`option_keys` test**: the GTK panel, the Windows launcher and the README
  option table may only name keys the engine understands, and the README
  must document every one; ten options found undocumented
  (`SMOOTHNESS`, `ZERO_BIAS`, `GRAD_WEIGHT`, `REFINE_BIAS`, `SYNC_PRESENT`,
  `HIDE_PRESENT_EXT`, `CAPTURE_HDR`, `FULLRES`, `UPSCALE`, `ENABLED`) now are.
- **Descriptor pools sized by construction.** `FrameGen` no longer keeps
  hand-written totals of every pass's descriptors (each optional pass had to
  add its sets to three formulas, and an undersized pool failed the
  creation and left the swapchain in pass-through): `makePass` registers
  what a set of its layout holds, and `allocSet` opens pools on demand
  through `DescriptorPlanner` (unit-tested). Output byte-identical.
- **Capture app: frame timing from the compositor's timestamps.** The
  capture interval (pacing, HUD) and the content interval (`frame_timing`,
  `dedup`) were measured when the capture thread dequeued each frame,
  adding that thread's wake-up jitter. They now use WGC's
  `SystemRelativeTime` (when the compositor produced the frame), converted
  by its age on the QPC clock so no clock epoch is assumed; the dequeue
  time remains the fallback.
- **`chroma_weight` refuses aliases that break the motion's course.** A
  chroma challenger must now stay within max(2, half the speed) pixels of
  the block's motion one pair earlier, unless there was no previous motion
  (so a motion can start). Stripes scrolling faster than their period match
  a different alias every pair and are filtered: demo −1.16 → −0.82 dB.
  The price, measured: isoluminant sequence +12.6 → +10.7 dB, swing
  +4.4 → +3.2, colourful pan unchanged (+11.7).
- **`dedup` detects partial repeats.** A game whose world runs at half the
  present rate under a HUD, cursor or counter redrawn every present never
  repeats the whole image, so nothing was detected. The duplicate test also
  counts differing pixels per tile of an 8×8 grid: a present where at most
  7 tiles changed right after one where at least 32 did is a repeat of the
  world (a static scene with a small moving object changes the same few
  tiles every present and is not). Offline, each image presented twice
  under a counter redrawn every present (`bdex_offline --hud-counter`):
  +0.3 to +6.9 dB, same as a full repeat (the whole-image test found none);
  no repeat detected on a static scene with a small moving object, and
  byte-identical on every non-repeating sequence.
- **Extrapolation: the real frame no longer waits for the synthesis.** It is
  presented first, but its copy was queued behind the analysis, flow and
  synthesis on the same queue. When the real frame is copied straight from
  the game's image (no upscaling, HUD or repeat replacement), the
  application thread now only consumes the game's semaphores and the
  worker submits the synthesis right after the real frame's copy and
  present (the game's present fence is signalled after the synthesis, which
  still reads its image). Measured on the demo, real frame ready after the
  game's present: balanced 3.5 → 1.5 ms, quality 3.7 → 0.8 ms — the
  synthesis' GPU time, larger on slower GPUs. The validation test gains an
  "extrapolate:direct" run for this path (0 messages).
- **Static blocks skip the block-matching search.** A block predicted at
  zero and identical in both frames (1-pixel border included) has a
  zero-motion candidate costing exactly 0, the unique winner of every
  selection path when the smoothness penalty is positive, so its search is
  skipped with a bit-identical result (all sequences, formats, modes and
  option sets checked against the previous build). Flow time on the demo
  (mostly static scenery, any GPU): balanced 1.75 → 1.30 ms, quality
  4.3 → 2.65 ms. Games with static HUDs, menus and scenery gain most.

- **`bdex_offline` benchmarks the engine**: `BDEX_FG_PROFILE=1` prints the
  mean GPU time per stage of a fixed workload (dump copies excluded), and
  `--bench N` repeats the flow back to back so the GPU stays at full clock
  (±7 % between runs instead of ±15–40 % for a live, half-idle demo). Used to
  settle the remaining flow optimisations, none kept: removing the smoothing
  pass and its barrier entirely saves nothing measurable (22.4 vs 22.4 ms
  per 20 flows on the demo), so fusing it into the refinement is not worth
  its complexity; a half-precision flow would save bandwidth on textures of
  only ~32 k texels (0.5 Mpx / 16) while needing new shader variants and
  breaking bit-exactness with `subpixel`. The static-block early-out above
  was the lever.

### Fixed (2026-09-25)

- MSVC warnings cleared (all from the CI logs): `_CRT_SECURE_NO_WARNINGS`
  for the standard `getenv`/`fopen` calls, explicit narrowing in the PPM
  dump, `_environ` no longer redeclared (inconsistent linkage),
  `WIN32_LEAN_AND_MEAN` guarded, a shadowed lock renamed. New option
  `BDEX_WERROR` (`/WX`, `-Werror`) on the layer, the unit tests and the
  capture app; both CI workflows turn it on.
- Windows capture build: the engine files in `layer/src/passes/` got the
  capture app's `log.h` (no `BDEX_INFO`) because `capture/src` came first
  among `bdex_capture`'s include directories; `layer/src` now comes first.
  `tools/ci/run.sh mingw` compiles the engine with that include order too.
- `eval_sequence.py`: SSIM was numerically wrong on large frames — the
  integral images were summed in float32 (squared values reach ~1e11 on
  2 Mpx), drowning the local variances in rounding noise: on the demo
  replay `subpixel=1` showed SSIM −0.06 for a +0.10 dB PSNR gain; now
  +0.0005. Summed in float64; earlier SSIM figures on full-size replays
  (not the small corpus) are not reliable.
- **Linux CI timeouts on slow runners.** GitHub's runners run lavapipe at
  ~2.4 fps under the sanitizers and the validation layers: the `offline`
  test (grown with the new schedule checks) passed its 120 s, the
  integration demo its 60 s, and the validation "state" runs (300 frames)
  their 120 s. The demo gains `--duration S` and every test run is bounded
  in time (40 s); `offline` uses shorter sequences and a 900 s limit.
  `tools/ci/run.sh` accepts `BDEX_CI_CPUS=n` to replay on a runner-sized
  container; on one CPU all jobs pass (ASan integration stops at 41 s).

- **Command pools were shared between threads.** Every virtual swapchain
  recorded its synthesis (game's presenting thread) and its copies (worker
  thread) in command buffers from the device's single pool, which Vulkan
  requires to be used by one thread at a time; with two swapchains, four
  threads did. The new `multithread_config` test (two swapchains, two
  presenting threads on two queues, the config file rewritten under them)
  crashed in the driver or hung; each swapchain now owns one pool per
  thread and the device pool (stage construction) is serialised. The test
  passes; it skips where a queue family has fewer than two queues.
- **Live config reload raced with other presenting threads.** A present
  reloading the config file rewrote the device's settings while another
  swapchain's present (or its synthesis recording) read them. Each virtual
  swapchain now works on a snapshot refreshed under the config mutex at
  the start of its present (`DeviceData::snapshotConfig`), the engine reads
  that snapshot, and the worker thread only reads values carried by its job.

- **`rot_weight` read outside its shared-memory tile** (undefined, run-to-run
  varying output): rotated candidates were placed with their absolute
  displacement instead of relative to the coarse prediction the tile is
  centred on, and the zero-motion candidate outside the search window was
  re-evaluated from the tile it is not in. Now deterministic. Offline demo
  replay after the fix: normal −0.27 dB, `--speed 2` +0.13 dB (was
  −0.18 / 0.00 with the bug); the option stays experimental.

### Fixed (2026-09-24)

- **`bdex-framegen --gl` did nothing on the NVIDIA driver.** It only set
  `MESA_LOADER_DRIVER_OVERRIDE=zink`, which reaches Mesa's own GL; with a
  vendor driver installed, libglvnd dispatched GLX/EGL to NVIDIA's GL, Zink
  never loaded and the game ran out of the layer's reach, silently. `--gl`
  now also selects the Mesa GLX/EGL vendor: `glxgears` on an RTX 3060 Ti
  goes through Zink, 880 → 1760 fps.
- **gamescope now works as a catch-all** (emulators, OpenGL/X11 apps): with
  its SDL backend it presents through one Vulkan swapchain, but it requires
  `VK_KHR_present_id`/`present_wait`, which the layer hides from games; a
  shipped `[gamescope]` profile exposes them (`hide_present_ext = 0`), so
  `enabled = 1` in a `[gamescope]` section is all it takes. Verified:
  `glxgears` inside gamescope 337 → 675 fps. The default Wayland backend
  presents dmabufs without a swapchain (documented: no generation there).
- **HDR swapchains are matched on the right scale.** The engine looked only
  at the swapchain format, never its colour space: an scRGB game (FP16,
  `EXTENDED_SRGB_LINEAR`, 1 = 80 nits, highlights at 10+) was matched on raw
  linear values, and HDR10 (PQ) on code values with white at 0.58. Costs,
  photometric errors and the scene-cut thresholds are tuned for a
  perceptual scale with SDR white at 1. HDR values are now converted to
  linear light relative to SDR white (203 nits) and encoded like decoded
  sRGB. Offline (`bdex_offline --colorspace scrgb|hdr10`, corpus with
  highlights up to ~1000 nits): scRGB HUD scene 10.8 → 20.0 dB (the raw
  costs tripped the scene cut), fade +0.55; HDR10 pan +2.8, fade +0.6.
  On SDR-range content both now score within 0.05 dB of the sRGB format
  (HDR10 subpixel pan was 34.5 → 44.5 dB). A PQ-domain scale was measured
  and rejected (mid-tones too flat: −3 to −10 dB to the zero bias). SDR
  swapchains are byte-identical. The `formats` test covers both.
- **Crash when a game exits under native Wayland** without destroying its
  device: the layer's global instance/device tables were torn down by static
  destructors at process exit, destroying the virtual swapchains after the
  Wayland connection was gone (segfault in `wl_proxy_marshal` from
  `vkDestroySwapchainKHR`). The tables are now intentionally never destroyed.

### Changed (2026-09-24)

- **Extrapolation no longer ghosts.** Where the forward/backward flow was
  inconsistent, extrapolation kept the pixel in place, so a moving object
  showed at its old position next to its warped copy — the double images
  reported in extrapolation mode (Kenshi, and the Windows capture app, which
  extrapolates by default). It now follows the warp everywhere, stretching
  disoccluded areas instead. Online `run_eval.sh` (extrapolate, 40 frames,
  Vega 8): normal 23.21 → 25.00 dB, `--speed 2` 19.68 → 20.70 dB, moving
  edges 13.09 → 15.61 dB, pixels revealed by the moving bar 7.58 → 20.32 dB.
  Offline: HUD scene +0.49 dB, fractal pan +1.0 dB. Looser consistency
  thresholds scored in between. `extrapolate_hold=1` restores the old
  behaviour. Interpolation is unchanged (byte-identical).

### Added (2026-09-24)

- **Both control panels expose the new options**: the GTK panel (Cadence:
  `frame_timing`, `dedup`, `gpu_budget`; Motion quality: `chroma_weight`,
  `scene_cut_local`; Experimental: `extrapolate_accel`) and the Windows
  launcher's "Options avancées" column (the same minus `gpu_budget`, which
  the capture app does not drive; rows tightened to 25 px to fit).
- **`gpu_budget`: dynamic quality under a GPU-time budget.** Auto-tuning
  picked a preset once, at start-up, from the GPU type; a heavy scene or an
  iGPU could still let the generation eat the game's frame time. With
  `gpu_budget = <share of the frame interval>`, GPU timestamps (always
  recorded when it is set) are averaged every second and a governor
  (`layer/src/budget.cpp`, unit-tested) steps down a ladder — costly opt-in
  passes off, narrowest search, flow at 1/2 then 1/4 resolution (output
  stage rebuilt) — after two windows over budget, and back up after five
  calm windows unless the level above is known not to fit. Demo at
  1272×1386 with the quality preset and a 2 % budget: 3.6 ms → 0.6 ms in
  four steps. The validation test walks the ladder, rebuild included, with
  0 messages.
- **`chroma_weight` (opt-in): the block matcher sees colour.** Matching ran
  on luma only, so an edge between two colours of equal brightness was
  invisible and those areas cross-faded. A Cb/Cr pyramid
  (`downsample_chroma.comp`) is built next to the luma one; where a block's
  luma is low-contrast, the candidates whose luma SAD is within 0.01 of the
  best are re-ranked on cost + weight × chroma SAD, and a challenger must
  halve the incumbent's chroma mismatch. Every guard was measured on the
  demo (−2.3 dB without them, all from its hatched box): the incumbent is
  the historical tree-reduction winner (§6 tie-breaking), textured luma is
  left alone, and small chroma differences do not switch. New corpus
  sequence `chroma` (isoluminant blobs): interpolation +12.6 dB,
  extrapolation +8.9; smooth colourful pan +11.8 / +8.8; swing +4.4 / +2.3;
  HUD, popup and sub-pixel neutral to +0.2. Remaining loss: nearly
  isoluminant stripes scrolling faster than their period (the demo's hatched
  box) lock onto an alias, −1.2 dB on the demo (+0.07 without that box).
  Default 0 is byte-identical (matcher test and identity checks).
- **`extrapolate_accel` (opt-in): second-order extrapolation.** Extrapolation
  assumed constant velocity, so a camera that speeds up, slows down or turns
  around overshoots and lags. The previous pair's finest backward flow is
  kept (one small image copy per frame) and each pixel follows a quadratic
  trajectory `s·v + a·s(s+1)/2`, `a` being the change of its motion since
  the previous pair — averaged over 3×3 neighbouring blocks that move alike
  (bilateral), gated by forward/backward consistency, ignored where the
  content's trajectory leaves the picture (entering content has garbage
  flow), below the flow's quantisation step, and limited to half the speed.
  Forward splatting follows the same trajectory. New corpus sequence
  `swing` (sinusoidal pan): x2 +1.0 dB, x3 +0.25; with
  `zero_bias_contrast=0.1` +2.7 dB. Other sequences ±0.05 except the
  smooth fast pan −0.13 (its flow is unreliable at the default zero bias);
  online demo `--speed 2` +0.13 dB (moving edges +0.35, revealed +0.8),
  normal −0.01. Each estimator step was measured: unfiltered per-block
  acceleration lost up to 0.9 dB on constant motion. Default 0 is
  byte-identical.
- **`scene_cut_local` (opt-in): scene cuts detected per region.** The
  scene-cut fallback used the mean matching cost of the whole frame: a menu
  opening over part of the screen was diluted (no fallback, warped panel)
  and a large fast object could fade everything. `reduce_cost` also averages
  an 8×8 tile grid; the synthesis blends between tile centres. New corpus
  sequence `popup` (a pause menu over a quarter of a pan, opening between
  two inputs): interpolation +1.0 dB, flicker −5.8; neutral on the other
  sequences and on the online demo (±0.04 dB), whole-frame cuts identical.
- **`frame_timing` and `dedup` (opt-in): generated frames follow the game's
  real timing.** A per-frame state pass on the GPU (`frame_state.comp`, no
  read-back before presenting) keeps a content clock: `frame_timing` gives
  every output the t of the moment it is displayed within the *actual*
  interval between the two real frames (the pacing spreads them over the
  *smoothed* one, so a frame-time spike used to misplace them); `dedup`
  detects images the game presents twice (level-0 luma, a few pixels of
  tolerance), gates the flow passes off through indirect dispatches so the
  previous pair's motion is kept, times the outputs on first appearances
  and, in extrapolation, replaces the stale repeated frame with an
  extrapolated one. Measured with the new `bdex_offline --jitter / --dup /
  --steady-content` schedules (every displayed frame scored against the
  frame the player should see): variable frame times ±20–40 % +0.1 to
  +2.3 dB; each image presented twice +0.9 to +7 dB (HUD scene extrapolated
  22.3 → 29.2 dB, flicker 48.6 → 7.8); a fixed-timestep game with jittery
  presents −0.6 dB at worst for `frame_timing` (hence opt-in). Both are
  byte-identical on a steady, non-repeating game. The demo gains
  `--repeat N`; the validation test runs both options (0 messages).
- **The Windows launcher gains an "Options avancées" column** with every
  engine option that applies to the capture app (motion quality,
  experimental, flow structure, debug view), table-driven, saved like the
  other settings (only non-default values; per-game profiles included).
  Checked with a MinGW build under Wine; the MSVC build comes from CI.
- **The control panel exposes every layer option.** Its advanced section is
  now six collapsible groups — Cadence, Motion quality, Experimental, Flow
  structure, Diagnostic, Compatibility — with switches, lists and text fields
  for the 38 options it did not show (only values that differ from the
  layer's defaults are written), and a *Paused* switch applied live to
  running games.

- `bdex_offline --format rgba8|bgra8|srgb|rgb10|rgba16f` and
  `--render-scale S`, and the `formats` ctest: every output encoding and the
  upscaling path are checked headless (BGRA8 byte-identical to RGBA8, 10-bit
  and FP16 within quantisation, sRGB within 0.15 dB against the ground truth,
  upscaled output within 1.5 dB of native). Frame dumps now also decode
  RGBA16F swapchains.
- `packaging/arch/PKGBUILD` (`bdex-framegen-git`, installs to `/usr`), and a
  Windows section in the README presenting both Windows products.

- **Shipped per-game profiles** (`layer/profiles.conf`, installed to
  `share/bdex-framegen/`, next to the DLL/exe in the Windows zip), loaded
  before the user's file. They only apply to named games and can never enable
  or pause the layer; a `[*]` section in them is ignored. First entry: Kenshi
  in interpolation (extrapolation artefacts reported on it). The anti-cheat
  list gains EasyAntiCheat_EOS, GameMon64 and the PunkBuster services.

- **Live configuration and an on/off toggle on Linux.** Running games check
  the config file every ~0.5 s and reload the settings that can change on the
  fly (flow/synthesis knobs, scene-cut thresholds, `fps_limit`, `target_fps`,
  sharpness between non-zero values) plus the new `paused` key, which
  presents only the real frames. `bdex-framegen --toggle` flips `paused` in
  the `[*]` section: bound to a desktop shortcut (e.g. a Hyprland `bind`), it
  is the Linux counterpart of the capture app's hotkey. Measured on the demo:
  x2 → x1 within 0.6 s of the toggle, and back. Settings that size resources
  still need a restart; environment variables keep priority.

- `target_fps`: dynamic multiplier. The frames presented per game frame
  follow target_fps × the game's frame interval, capped at `multiplier`
  (the output stage is sized for it), with a ±0.15 hysteresis around the
  switch points. Demo, target 144, max x4: 30 fps → x4, 50 → x3, 72 → x2,
  150 → x1.
- `fps_limit` (alias `fps_cap`): built-in frame-rate cap for the game, in
  the application's present (sleep to ~1.5 ms before the slot, then a short
  spin; after a late frame the schedule restarts instead of bursting).
  Demo at 120 fps capped to 45: game 45.0 fps → output 90.0 fps. The control
  panel exposes it, together with `zero_bias_contrast`, `pixel_vectors`,
  `subpixel` and `extrapolate_splat`.

- `bdex_offline` + `tools/eval_sequence.py`: offline quality evaluation on
  any recorded frame sequence (a game dumped through the layer with
  `BDEX_FG_MULTIPLIER=1`). The unchanged engine runs headless (no window, no
  compositor-dependent size); every `multiplier`-th frame is an input, the
  others are the ground truth. Reports whole-frame / moving-region /
  moving-edge PSNR next to frame repetition and cross-fade baselines, and
  A/B-compares `--variant` options on identical inputs. On the demo it
  reproduces the layer's online frames exactly (outside the frame counter).
  New `offline` ctest (synthetic panning sequence, no display needed).
- `static_guard` (opt-in, default 0 = bit-for-bit): in interpolation, a pixel
  identical in both frames keeps the current frame, so static HUD elements
  over moving content no longer smear with their block's motion. Offline
  (Vega 8, x2): HUD-over-pan scene +0.98 dB (minimap +4.4, text box +1.1),
  demo +0.01 dB, demo `--speed 2` −0.20 dB. A 3×3 test never fires on
  see-through HUDs (a glyph's neighbours move); conditioning on warp
  consistency removed most of the HUD gain; in extrapolation the guard costs
  0.5–0.8 dB on the demo, so it is interpolation-only.
- `pixel_vectors` (opt-in): per pixel, the interpolation also tries the
  unblended vectors of the four flow texels around each trajectory's source
  and keeps the one whose endpoints agree best — the bilinear flow lookup
  blends two motions into a wrong vector at object boundaries. Online
  `run_eval.sh` (40 frames, Vega 8): normal 29.48 → 29.87 dB, `--speed 2`
  23.26 → 23.83 dB; moving edges 22.13 → 23.14, revealed pixels +1.1 dB
  (fast +4.0), rotating edges 27.18 → 31.89 dB. With `subpixel` and
  `zero_bias_contrast=0.1`: 29.80 / 24.33 dB. Offline: HUD scene +0.64 dB,
  no loss anywhere. Synthesis GPU time about doubles (1.4 → 2.8 ms).
- `extrapolate_splat` (opt-in): extrapolation forward-splats every pixel of
  the current frame to its predicted position (atomicMin on a priority key:
  consistent, faster-moving pixels win) and falls back to the backward warp
  only where nothing lands. Online `run_eval.sh` (extrapolate, 40 frames,
  Vega 8): normal 25.00 → 26.08 dB, `--speed 2` 20.70 → 21.11 dB, moving
  edges 15.61 → 17.17 dB; offline x3 +1.05 dB, HUD scene +0.24 dB; with
  `zero_bias_contrast=0.1` offline demo +1.72, fast +0.99 dB. Cost: synthesis
  0.67 → 1.45 ms on the Vega 8.
- `static_guard` now requires the flow at the pixel itself to indicate motion
  (a static pixel inside moving content is an overlay; a static background
  pixel that a small object crosses has zero flow there), with an 8× stricter
  "unchanged" threshold. Offline: HUD scene +0.95 dB, demo +0.04 (was +0.01),
  fast −0.13 (was −0.20), sub-pixel smooth pan −2.6 dB (−7.7 with the old
  threshold).
- `zero_bias_contrast` (opt-in, default 0 = bit-for-bit): scales the zero
  bias by min(1, E / zero_bias_contrast), E the block's mean luma gradient.
  At 0.1, online `run_eval.sh` (40 frames, Vega 8): normal 29.48 → 29.50 dB,
  `--speed 2` 23.26 → 23.74 dB (rotating edges 18.61 → 21.29 dB); offline:
  HUD scene +0.03, smooth pan +17.2 dB, extrapolation +0.06..+0.44 dB. The
  first matcher change measured non-negative in both regimes that also fixes
  the smooth-content failure; `zero_bias=0` gains more on fast motion
  (+0.75) but loses 0.10 at normal speed. 0.2–0.4 behave like `zero_bias=0`.
- `subpixel` (opt-in, default 0 = bit-for-bit): parabolic fit of the finest
  flow on an 8×8 SAD window around each 4×4 block's winner. Online
  (40 frames): normal +0.08 dB, `--speed 2` +0.06 dB, moving edges
  +0.1–0.2 dB; with `zero_bias_contrast=0.1`: 29.58 / 23.80 dB vs 29.48 /
  23.26 (rotating edges +2.4 dB). Offline sub-pixel pan +0.42 dB. The 2026-09
  attempt (−0.1 dB) fitted the 4×4 SAD under the absolute zero bias.
- `temporal_prediction` (opt-in, default 0 = bit-for-bit): the coarsest
  block-matching level searches around a constant-velocity prediction from
  the previous frame pair (minus its backward flow, which is indexed by the
  current reference frame) instead of around zero. Offline, fractal pan at
  64 px per input pair: 23.51 dB → 23.82 alone, **35.95 with `zero_bias=0`**;
  demo ±0.0, fast +0.07. An extra candidate at every level instead lost
  0.1–0.5 dB on fast motion.

### Measured (2026-09-24)

- **`zero_bias` is what breaks fast motion on low-contrast content.** Its
  per-pixel displacement penalty does not scale with the block's contrast, so
  on smooth textures the costless zero-motion candidate wins and the frame
  cross-fades (ghosting). Pure pan at 32 px per pair on a fractal texture:
  28.48 dB default → 46.21 dB with `zero_bias=0` (only 46% of the blocks got
  the right motion by default). Demo, online `run_eval.sh` 40 frames:
  normal 29.48 → 29.38 dB, **`--speed 2` 23.26 → 24.01 dB** (rotating edges
  18.61 → 21.41 dB); offline: normal −0.13, fast +0.89, HUD scene +0.09.
  Not changed by default yet (it moves the byte-for-byte CI reference).

### Improved (2026-09-24)

- Windows capture now applies its extrapolation default before config/env and
  respects explicit interpolation, including in the launcher. CLI mode and
  multiplier take precedence over presets. Windows CI pins and verifies the
  Vulkan SDK version and checksum.
- Experimental `occlusion_strength=1` now reduces fallback cross-fading in
  uncertain interpolation regions. The quality evaluator reports revealed
  pixels and rotating edges and stores dumps compactly; normal and fast demo
  runs gained in those regions while the default output stayed byte-identical.
- Virtual swapchain images retain every usage requested by the game. If an
  ordinary image of that format cannot provide those usages, the layer keeps
  the driver's swapchain. Multi-swapchain presents now reindex the known
  per-swapchain `pNext` arrays for each forwarded present; a two-swapchain
  unit test covers IDs, modes, regions, fences and device masks.
- The quality runner ignores the user's config file while preserving explicit
  `BDEX_FG_*` overrides. Reports show how many generated frames aligned with
  ground truth, and runs with too few comparisons fail instead of publishing
  a misleading PSNR mean.

### Improved (2026-09-23)

- Virtual swapchain acquisition now respects zero, finite and infinite Vulkan
  timeouts, waits for images released by the application or the GPU, wakes on
  worker/resize errors even if a notification races with the wait, and rolls
  back the reservation when signalling the acquire semaphore fails. A Vulkan
  integration test exhausts the virtual images and
  checks `VK_NOT_READY`, `VK_TIMEOUT`, and reacquisition after presentation
  under Xvfb/software Vulkan.
- A game explicitly requesting present-timing extensions keeps its original
  device features and uses a pass-through swapchain. The layer no longer
  writes into the application's `pNext` feature chain. If the preferred
  compute queue cannot present, the layer can use a compatible application
  queue before its first virtual swapchain.
- The Windows capture thread is joined before its frame pool and ring are
  destroyed; the WGC callback now owns its wake event independently of
  `Capture` lifetime. Capture logs mean/p95 WGC-frame-dequeue-to-synthesis and
  dequeue-to-present-API times for successfully submitted real frames.
- Frame dumping and the quality runner now cover every generated position at
  x2/x3/x4. The report adds PSNR for changing regions and moving edges.
  Those local masks exclude the demo's frame counter used for alignment.
  `occlusion_strength` is an opt-in interpolation experiment using forward /
  backward flow consistency; its default of zero retains the older synthesis.
  Initial normal/fast GPU evaluations showed no whole-frame PSNR gain at full
  strength, so this remains experimental.
- Linux CI builds the layer, runs unit and GPU integration tests under Xvfb,
  and compares default generated frames with the pre-census build to guard
  exact tie-breaking. It also checks generous normal/fast PSNR floors at x2/x3/x4.
  Test runners isolate the build-tree layer from any installed copy.
  The integration test skips unavailable displays locally but must run in CI.

### Added

- **Windows capture (mode 2): global toggle hotkey.** **Ctrl+Alt+G** turns
  frame generation on and off at runtime without stopping the capture (off =
  the real captured frames pass through unchanged, `multiplier` 1, minimal
  latency). Rebindable through a new `hotkey` config key (e.g.
  `hotkey = ctrl+shift+f1`; modifiers `ctrl`/`alt`/`shift`/`win` + a letter,
  digit, or `F1`–`F24`). Ctrl+Alt+Q still quits. A busy combo is a warning,
  not a failure.

- **Windows capture (mode 2): per-game profiles.** `bdex_capture` now applies
  the config file's `[game]` section for the *captured* title, not just its
  own process — a hand-written `[DarkSoulsIII]` section (or one saved from the
  launcher) overrides the globals for that game. The launcher gained a
  **"Profil pour ce jeu"** checkbox that writes the options into the target's
  `[game]` section (preserving the global scope, other games' sections and
  hand-written keys); selecting a game with an existing profile loads it and
  ticks the box. Implemented as an `extraProcess` argument to
  `Config::load()` (the layer path is unchanged, bit-for-bit).

- **Linux CI** (`.github/workflows/linux.yml`): every push builds the layer
  and demo on Ubuntu, runs the unit + integration tests under Xvfb on
  lavapipe (software Vulkan — the runners have no GPU) and gates the PSNR
  quality eval, failing below 28.0 dB normal / 23.0 dB fast (`balanced`).
  lavapipe tracks the hardware baseline to within ~0.04 dB (measured
  28.87 / 23.49 dB vs 28.91 / 23.52 on a GPU), so the gate sits just under
  the real numbers — tight enough to catch a matcher regression, with
  headroom for renderer noise.

- **Rotation/scale re-ranking for the block matcher** (opt-in `rot_weight`,
  default 0 = bit-for-bit off): when a block's match is ambiguous (runner-up
  within 50% of the winner) and the winner does not explain the block well
  (mean SAD above 0.02), the top-2 displacements are re-evaluated under 15
  local transforms — scales 0.92/1.0/1.08 × angles −30/−10/0/+10/+30° about
  the block centre, sampled bilinearly from the tile already staged in shared
  memory — and the winner is switched only if a transformed candidate beats
  it by 0.5%. The flow output stays a translation; the transform only
  re-ranks candidates, so the extra cost is paid only on ambiguous,
  non-static blocks. Measured on the quality eval (2026-09-20, RTX 3060 Ti,
  vs the same build with `rot_weight=0`): **+0.25 dB normal, +0.32 dB fast
  motion (`--speed 2`), +0.11 dB at `--speed 2.5`** at `balanced` (+0.28 dB
  fast at `quality`) — a consistent gain on both regimes, unlike the
  opt-in `grad_weight`/`census_weight` knobs. Unlike those two it is also
  non-negative on static scenes by construction (confident and static blocks
  skip the re-ranking), but it stays opt-in until validated on more titles:
  `rot_weight = 1` globally or in a game's config section.

- **Census re-ranking for the block matcher** (opt-in `census_weight`, default
  0 = bit-for-bit off; `census_margin`, default 0.05): when the runner-up
  candidate scores within the margin of the winner, the near-winners are
  re-ranked on census Hamming distance — an 8-bit rank transform (bit k =
  neighbour k ≥ centre) computed from the tiles already staged in shared
  memory, with no extra pass or bandwidth. Measured on the quality eval:
  broad re-ranking (`census_weight=2`, `census_margin=1`) gains ≈ +0.5 dB on
  fast motion (`--speed 2`) but costs ≈ −1.5 dB on static/small-motion scenes;
  tight gating is the only non-negative setting (+0.12 dB normal). Same
  trade-off pattern as `grad_weight`, hence the same opt-in default: for
  motion-heavy games, `census_weight=2` + `census_margin=1` in the game's
  config section.

### Changed

- `DeviceData::swapchains` holds `VirtualSwapchain`s through a
  function-pointer deleter, so `device.cpp` and `FrameGen` compile without
  the layer's swapchain virtualisation (the capture product reuses them).

### Fixed

- **Linux CI brought up green** (bring-up of the workflow above): the
  `Dependencies` step installed a non-existent package (`glslc-tools`) — the
  `glslc` binary that `find_program(GLSLC glslc REQUIRED)` needs ships in the
  `glslc` package on Ubuntu noble. lavapipe is then selected robustly: the ICD
  path is resolved with a glob (hardcoding `VK_ICD_FILENAMES` at a fixed path
  made the loader find no driver at all — a bare `vkCreateInstance` -9 — on
  runner images where the file lives elsewhere) and verified with `vulkaninfo`
  so a broken environment fails fast. Finally the layer is registered in the
  global `implicit_layer.d` (the canonical discovery path every loader honours,
  and how the layer ships) rather than only via `VK_ADD_IMPLICIT_LAYER_PATH`,
  which some loader builds ignore — so the integration test's demo actually
  loads the layer instead of running pass-through ("layer did not activate
  frame generation").

- **Block-match winner selection restored to bit-for-bit at `census_weight=0`.**
  The census commit's sequential winner scan broke cost ties differently from
  the original tree reduction (left subtree wins on a tie), so even with the
  feature off (the default) the flow — and the generated frames — differed
  from the pre-census build; flat / periodic regions pick among exactly tied
  candidates. The default path now runs the original per-thread minimum + tree
  reduction again (same costs, comparisons, tie-breaking); the sequential
  scan and re-ranking are used only when `census_weight > 0`.

- **Quality eval harness made reproducible.** The demo created its first
  swapchain while the window manager was still mapping/resizing the window,
  so `tools/run_eval.sh` intermittently produced sequences at two different
  sizes ("no comparable frames"), and a tiling WM re-tiled concurrent demo
  windows to half height. The demo now waits for the framebuffer size to
  settle before creating the swapchain and pins the window size
  (`GLFW_RESIZABLE=false`) under `--deterministic`; `run_eval.sh` pins
  `BDEX_FG_PRESET=balanced` unless the caller sets one, so a user config file
  no longer silently changes the measured operating point.

- **Windows layer** `log_file` now expands environment references before
  opening the file: `%VAR%` on Windows (via `ExpandEnvironmentStringsA`),
  `$VAR` / `${VAR}` / a leading `~` on Linux. The Windows docs recommend
  `log_file=%TEMP%\bdex-fg.log`, which was being opened verbatim — so the
  layer logged nowhere (and a game's stderr goes nowhere on Windows), leaving
  a first run with no diagnostics. Covered by a new `config_expand_log_file`
  unit test on both platforms (`bdex::platformExpandPath`).
- `bdex_capture`'s overlay no longer paints its own arrow cursor over the
  game: on titles that hide the hardware cursor (PRAGMATA, Dark Souls III)
  the overlay's class cursor stayed visible on top of the game (double /
  lingering cursor). The overlay now has no class cursor and never sets a
  shape in `WM_SETCURSOR`; when the game has hidden the hardware cursor
  (`GetCursorInfo`), the cursor is force-hidden over the overlay too. Found
  during the phase-2 first run on hardware (2026-09-15).

### Added

- **Windows capture product** (`capture/`, mode 2 of the Windows plan,
  phases 1–2 of `docs/windows-capture.md`): `bdex_capture.exe` captures any
  windowed game with Windows.Graphics.Capture, shares the D3D11 frames into
  Vulkan (`VK_KHR_external_memory_win32` + `VK_KHR_win32_keyed_mutex`), runs
  the layer's frame-generation engine on them (`FrameGen` and the shaders,
  unchanged: optical flow, x2/x3/x4 interpolation or extrapolation,
  upscaling, HUD) and presents real + generated frames, paced like the
  layer, in a topmost overlay that follows the game window (`--list`,
  `--title`/`--pid`, `--scale`/`--fit`, `--multiplier`, `--mode`,
  `--preset`, `--filter`, `--sharpness`, `--hud`, `--gpu`, `--fifo`,
  Ctrl+Alt+Q; the config file and `BDEX_FG_*` apply). The discrete GPU is
  preferred and the D3D11 device is created on the same adapter. MSVC/x64
  only. Phases 1 (capture/interop/present) and 2 (frame generation on the
  captured frames) validated on hardware (2026-09-15: PRAGMATA, Dark Souls
  III, Skyrim SE, Fallout: New Vegas — D3D12/11/9).
- **Windows launcher GUI** (`capture/src/launcher.{h,cpp}`): `bdex_capture.exe`
  run without arguments opens a native Win32 window (no toolkit): pick the
  game window, set the same options as the Linux control panel (generation
  on/off, ×2–×4, mode, quality preset, overlay size / fit, upscale filter,
  CAS sharpness, FIFO, fps HUD) and Démarrer persists them to the shared
  `bdex-framegen.conf` — game sections and unknown keys preserved — and
  starts the capture; when the capture stops the launcher reopens.
  `present_mode = fifo` in the config now also selects FIFO in capture mode.
- **Windows CI** (`.github/workflows/windows.yml`): MSVC build of the layer
  (x64 + Win32) and the capture PoC, unit tests, one zip artifact per
  architecture, GitHub release assets on `v*` tags. `docs/windows-README.md`
  ships in the zip.

- **Windows port of the Vulkan layer** (mode 1 "injection" of the two-mode
  Windows plan; see `docs/windows-port.md`). The layer builds with MSVC
  (`VkLayer_bdex_framegen.dll`, `__declspec(dllexport)` entry points, no ELF
  linker flags), reads its config from `%APPDATA%\bdex-framegen\bdex-framegen.conf`,
  matches `[game]` sections against Windows process names, and installs per-user
  via the registry with `tools/register-layer.ps1` /
  `tools/unregister-layer.ps1` (no admin; 32-bit layer under `WOW6432Node`).
  Platform differences live in the new `layer/src/platform.{h,cpp}`.
- **Anti-cheat auto-retract**: when a known multiplayer anti-cheat process is
  detected (EasyAntiCheat, BattlEye, GameGuard, XIGNCODE3, miHoYo — Vanguard
  excluded, see the comment in `platform.cpp`), the layer forces itself off for
  that process regardless of config/env, and logs it. The check runs on Linux
  too (catches the Wine/Proton case via `/proc`), where it also protects
  Proton multiplayer titles.

- Spatial upscaling now works on **native Wayland**, not just X11/Xwayland. A
  Wayland surface never reports its size (`currentExtent` is always
  `UINT32_MAX`), so the layer learns the true window size at the first
  swapchain creation, bounces the app through one swapchain recreation
  (`VK_ERROR_OUT_OF_DATE_KHR`, the normal resize path), and then reports a
  reduced `currentExtent` so the app renders smaller and every frame is
  upscaled back to the window size. Window resizes re-learn the new size.
- Hook `vkGetPhysicalDeviceSurfaceCapabilities2KHR` as well as the base
  variant, so the render-scale reduction also reaches apps that query surface
  capabilities through `VK_KHR_get_surface_capabilities2` (DXVK/VKD3D-Proton),
  on both X11 and Wayland.
- Demo: `--ignore-resize` keeps the swapchain through window resizes and
  `VK_SUBOPTIMAL_KHR`, like some games do, to exercise the case above.
- The on-screen fps counter (`OVERLAY`/`HUD`) now has selectable detail levels:
  `1` output fps, `2` `game > output`, `3` adds a `1%` low line, `4` adds a
  `.1%` low line (also `off`/`fps`/`framegen`/`low`/`full`). The 1% / 0.1% lows
  are the game fps at the 99th / 99.9th percentile of the game's frame time,
  computed over a rolling window; low lines are drawn in amber. Selectable from
  the control panel ("Compteur de FPS") and the launcher (`-o/--hud`).

### Changed

- The layer is now always loaded into Vulkan games (the `BDEX_FG=1`
  `enable_environment` gate was removed from the manifest) and stays a pure
  pass-through until it is turned on — so frame generation can be enabled from
  the config file alone, with **no Steam launch options**. The default state is
  now off (`enabled = false`); `BDEX_FG=1` in the environment still forces it on
  for a single process and overrides the config, keeping the old per-game and
  command-line workflows working.
- The GTK graphical launcher is now a **control panel** (Lossless-Scaling
  style): a master "enable for all Vulkan games" switch plus a per-game list you
  add to, backed directly by `~/.config/bdex-framegen.conf` (read on start,
  written on every change, hand-written keys preserved). Open it once, turn
  frame generation on, and launch games normally through Steam.

### Fixed

- Upscaling (`UPSCALE` / `RENDER_SCALE`) presented only the top-left
  render-sized region of the display-sized frame: the copy into the real
  swapchain used the render extent, so a 720p game upscaled to 1080p showed a
  cropped 1280x720 image with the rest of the screen black. The copy now
  covers the display extent.
- The real swapchain follows the surface when the window is resized behind
  the game's back (X11 / Xwayland, e.g. Wine applying an emulated display
  mode): on `VK_SUBOPTIMAL_KHR` the layer re-creates its real swapchain and
  output stage at the surface's current extent instead of presenting cropped
  or padded frames; the game's virtual swapchain is untouched.

## [0.3.0] - 2026-09-14

### Added

- On-screen frame-rate HUD (`OVERLAY=1`, alias `HUD=1`): a small
  "game fps > output fps" readout drawn in the top-left of every presented
  frame — real and generated — so the effect of frame generation is visible in
  the game.

### Fixed

- Frame generation now works with games running under Proton (Direct3D through
  DXVK / VKD3D-Proton), verified generating x2 in real titles. The virtualised
  swapchain broke several assumptions those layers make:
  - `VK_SUBOPTIMAL_KHR` from the layer's own real swapchain is no longer
    forwarded to the game. It says the layer's real swapchain is not optimal,
    not the game's virtual one; forwarding it made DXVK recreate its swapchain
    and hang. A genuine change still surfaces as `OUT_OF_DATE`.
  - The present-timing extensions (`VK_EXT/KHR_swapchain_maintenance1`,
    `VK_KHR_present_id`/`present_wait` and their `*2` variants) are hidden from
    the game: their semantics refer to the real swapchain the layer virtualises
    and the game hangs driving them through the asynchronous presentation.
  - FIFO is never used for the real swapchain (it hard-hangs the async present
    with these layers and adds latency); the layer presents with mailbox, or
    immediate when mailbox is unavailable, keeping its own pacing.
  - The application's `VK_EXT_swapchain_maintenance1` present fence, when the
    game still uses it, is signalled after the layer's copy (the virtual image
    is free then) rather than tied to the real frame's display.
  - `vkWaitForPresentKHR` / `vkGetSwapchainStatusKHR` are handled for the
    virtual swapchain so the game never blocks on presentation the worker owns.

## [0.2.0] - 2026-09-13

### Added

- Hardware auto-tuning (`PRESET=auto`, now the default): the flow quality is
  chosen from the detected GPU at device creation — integrated, software and
  small/old GPUs get the cheaper `performance` flow, discrete GPUs the
  `balanced` flow (both still resolution-adaptive). Setting any preset or flow
  option turns it off. The graphical launcher shows the detected GPU and a
  quality-profile selector.

- Spatial upscaling (`RENDER_SCALE=0.5..1.0`, or `UPSCALE=<ratio>`): the layer
  advertises a reduced surface size so the game renders fewer pixels, then
  resamples every presented frame — real and generated — to the display
  resolution. `UPSCALE_FILTER=bilinear|bicubic|lanczos` (default Lanczos-2)
  selects the reconstruction filter; generated frames are synthesised directly
  at display resolution. Works with or without frame generation. Requires a
  fixed-size surface (X11 / Xwayland, i.e. most games via Proton); Wayland
  surfaces that let the client choose the size are presented unscaled.
- `SHARPNESS=0..1`: a Contrast-Adaptive Sharpening (CAS) pass applied after
  upscaling, to both the real and the generated frames. The sharpening amount
  adapts to local contrast so flat and already-sharp areas are left untouched
  (no ringing). Runs on a linear rgba16f intermediate; off by default.

- `MODE=extrapolate`: the real frame is presented immediately and the
  generated frames are predicted from the motion of the last two, removing
  the half-frame delay of interpolation. Predicted frames still pending when
  the next real frame arrives are skipped.
- `PRESET=quality|balanced|performance|latency`, `FLOW_SCALE=auto|1|2|4`
  (replaces `FULLRES`, kept as an alias), `REFINE_ALL`, `FLOW_ITERATIONS`,
  `LOW_LATENCY`. `PRESET=latency` (also `-P latency`) selects extrapolation at
  x2 with the default mailbox present mode: the real frame is held ≈ 0.1 ms
  instead of the ≈ 8 ms (half a frame) that interpolation adds.
- `real frame delayed` latency measurement in the statistics; GPU profiling
  now covers the worker's copies.
- Launcher options `-x/--extrapolate`, `-P/--preset` and `--gl` (OpenGL
  games through Zink).
- Graphical launcher `bdex-framegen-gui` (GTK 4 / libadwaita): pick a game and
  set every option with switches and dropdowns, then launch it, copy the Steam
  launch options, or save a per-game profile. Installed with a desktop entry;
  also reachable as `bdex-framegen --gui`.

### Changed

- `PRESENT_MODE` defaults to `auto`: a game asking for FIFO is presented with
  mailbox plus pacing, which removes the 2-3 refreshes of latency that FIFO
  queueing added (`app` keeps the game's mode).
- The images the game renders into are used directly as the frame history:
  one full-resolution copy per frame and two history images less. The game
  gets one more swapchain image in exchange.
- The real swapchain has `multiplier + 2` images so that the real frame's
  acquire does not block behind the generated ones.
- Flow refinement compares 4×4 blocks instead of 6×6 windows (faster and
  slightly better), the interpolation uses one fixed-point iteration by
  default (two were measured to bring nothing).
- Each warp trajectory in the interpolation now blends its samples from both
  the previous and the current frame (temporal blend, gated by the
  trajectory's own photometric consistency so an occluded sample cannot
  contaminate it) instead of using a single frame. Consistently better across
  motion speeds at no extra texture fetch.

## [0.1.0] - 2026-09-12

First release.

### Added

- Implicit Vulkan layer `VK_LAYER_BDEX_framegen`, enabled with `BDEX_FG=1`,
  which virtualises the game's swapchain and presents intermediate frames
  (x2, x3 or x4 multiplier).
- GPU motion estimation: luma pyramid (R16F when the driver supports it),
  hierarchical 8×8 forward and backward block matching with a bias toward
  zero motion, 3×3 median filter, 4×4 block refinement.
- Intermediate frame synthesis by bidirectional warping with a fixed-point
  search of the source position, weighting by photometric consistency,
  cross-fade in unexplained regions and scene cut detection (thresholds
  measured on the demo).
- Presentation from a dedicated thread on a compute queue separate from the
  game's; pacing of generated frames in mailbox / immediate modes; mailbox +
  pacing fallback when the queue has to be shared.
- Support for the RGBA/BGRA 8-bit (UNORM and sRGB), A2B10G10R10,
  A2R10G10B10 and RGBA16F swapchain formats; transparent pass-through of
  other formats and of multi-layer or protected swapchains.
- Forwarding of `VK_KHR_present_id`, of `VK_EXT_swapchain_maintenance1`
  present fences and present mode switches, and of
  `vkReleaseSwapchainImagesEXT`; handling of `oldSwapchain` (the old
  swapchain is retired before being replaced).
- Configuration through `BDEX_FG_*` variables and the
  `~/.config/bdex-framegen.conf` file with per-game sections (`[Game.exe]`).
- `flow`, `split` and `passthrough` debug modes, periodic statistics,
  per-stage GPU profiling (`PROFILE`), export of the presented frames
  (`DUMP`).
- GLFW demo application (`bdex_demo`) with a procedural scene, frame counter,
  deterministic mode, adjustable scene cuts and speed.
- Unit tests (configuration, formats), end-to-end test (doubled output frame
  rate) and quality evaluation tool (`tools/run_eval.sh`, PSNR against a
  60 fps reference).
- `tools/install.sh` / `tools/uninstall.sh` scripts (64-bit and 32-bit),
  `bdex-framegen` launcher with a `--check` option.

### Fixed

- Dispatch pointer of objects created by the layer (queue, command buffers),
  without which layers below it (Khronos validation) aborted.
- Shaders compiled for SPIR-V 1.0 so that Vulkan 1.0 applications (`vkcube`)
  are accepted.
- Swapchain image layout transition ordered after the acquire semaphore's
  wait stage (synchronization validation).

[Unreleased]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Bdexez/bdex_frame_gen/releases/tag/v0.1.0
