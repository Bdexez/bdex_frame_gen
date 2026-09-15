# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- `DeviceData::swapchains` holds `VirtualSwapchain`s through a
  function-pointer deleter, so `device.cpp` and `FrameGen` compile without
  the layer's swapchain virtualisation (the capture product reuses them).

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
  only. Phase 1 (capture/interop/present) validated on hardware.
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
