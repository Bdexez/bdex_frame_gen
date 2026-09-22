# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added (2026-09-24)

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
