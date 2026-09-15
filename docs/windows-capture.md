# Windows capture-based product — scope

**Mode 2 of the two-mode Windows plan** (decided 2026-09-14): this is the
**multiplayer-safe** path. Mode 1 is the [Vulkan-layer injection](windows-port.md)
(lowest latency, solo games / emulators, small anti-cheat risk). This document
scopes a **Windows application** that delivers the same rendering
as bdex-framegen (optical-flow frame generation + spatial upscaling) but via
**screen/window capture** instead of a Vulkan layer — the Lossless Scaling
model — so it works with **every render API** (D3D9/10/11/12, OpenGL, Vulkan)
without injecting into the game, which is what multiplayer anti-cheats flag.

It reuses bdex-framegen's **algorithm and shaders**, not its layer machinery.

---

## 0. The honest trade-off up front: capture adds latency

This must be clear before committing, because it is the opposite of the current
Linux strength:

- The Linux implicit layer is **in the game's present path**, so it adds the
  least possible latency (and `MODE=extrapolate` adds ~0 ms).
- A **capture** product is **after** the present path: you receive a frame only
  once the game has already shown it, then you process and re-present it. That
  is **inherently ~1 extra frame of latency**, before any frame-gen hold. This
  is true of Lossless Scaling too — it is the price of being API-agnostic
  without injecting into the game.
- So on Windows, capture buys **coverage of every D3D game** at the cost of
  **more latency than the Linux layer**, not less. Frame-gen mode still matters:
  interpolation adds another ½-frame hold; **extrapolation** keeps it to roughly
  the capture cost. We should default to extrapolation here.

Two ways exist to reduce that cost, each with a catch:

| Approach | Latency | API coverage | Anti-cheat | Effort |
|---|---|---|---|---|
| **External capture** (WGC / Desktop Duplication) — LS default | highest (~1+ frame) | all APIs, no per-game work | safest (no injection) | medium |
| **Present-hook injection** (hook `IDXGISwapChain::Present`, D3D9 `Present`, `wglSwapBuffers`, `vkQueuePresent`) | lower (in present path) | per-API hooks, one per API | **risky** (injection is what anti-cheats flag) | high (N hooks) |

**Recommendation:** build on **external capture** (WGC primary, Desktop
Duplication fallback). It is API-agnostic, injection-free, and matches LS. Accept
that latency is higher than the Linux layer and be honest about it in the UI.
Present-hook injection can be a later, opt-in "low-latency mode" for specific
single-player titles, but it is a separate, fragile track.

---

## 1. What we reuse from bdex-framegen

The **algorithm is the asset**, and it is a good fit: bdex-framegen already
estimates motion **from colour only** (luma pyramid + block-matching optical
flow), which is exactly the situation a capture tool is in — no motion vectors,
no depth, just the final image.

Reused (with a new host around them):

- **Every compute shader** in `layer/shaders/`: `downsample`, `block_match`,
  `flow_smooth`, `flow_refine`, `reduce_cost`, `interpolate`, `upscale`,
  `cas`, and `common.glsl` (including the fps HUD).
- **The flow + synthesis orchestration** in `framegen.cpp` (pyramid build,
  forward/backward flow, refinement, interpolation/extrapolation, upscaling,
  sharpening, pacing math) and its tuning (`config.cpp` presets, scene-cut, the
  eval-validated defaults).

**Not** reused: the Vulkan-layer machinery — `layer.cpp` (loader negotiation,
dispatch, `vkGetInstanceProcAddr` hooks), the swapchain **virtualisation**
(`swapchain.cpp` intercepting the game's swapchain), the manifest/registry
install. Capture replaces all of that.

### Where the shaders run on Windows — two options

The shaders are GLSL → SPIR-V, i.e. a Vulkan compute pipeline. Capture, however,
hands us **D3D** textures. Two ways to bridge:

1. **Keep Vulkan compute, interop the captured texture (recommended).** Vulkan
   runs on Windows. Capture (WGC/DupliAPI) → D3D11 texture → share it into
   Vulkan via `VK_KHR_external_memory_win32` + keyed mutex, run the **existing
   SPIR-V shaders and `framegen.cpp` logic unchanged**, present via a Vulkan
   Win32 swapchain. **Reuses ~90% of the processing code.** Cost: the D3D↔Vulkan
   interop plumbing (well-documented, but fiddly around keyed-mutex sync).
2. **Port the pipeline to D3D12/HLSL.** Rewrite all 8 shaders in HLSL and the
   orchestration in D3D12. No interop (capture is already D3D), one clean API,
   but it re-implements the whole processing stack and forks the shader code
   from the Linux tree.

**Recommendation:** option 1 — Vulkan compute + D3D interop — to keep a single
shader/algorithm codebase shared with Linux. Revisit only if interop sync proves
unreliable across vendors.

---

## 2. New components to build (the bulk of the work)

This is a new application; the reused algorithm is maybe 30% of it. The new 70%:

### 2.1 Capture
- **Windows.Graphics.Capture (WGC)** as primary (Win10 1803+): capture a chosen
  window or monitor into a D3D11 texture, per-frame, with a frame pool. Handles
  most cases and is the modern path.
- **DXGI Desktop Duplication** as a fallback (older, monitor-only, exclusive-
  fullscreen quirks).
- Handle: window vs fullscreen selection, the cursor, DPI/scaling, HDR, and
  detecting the game's real present rate (to know when a new frame arrived).

### 2.2 Presentation / overlay
- Present the processed + generated frames in a **borderless, always-on-top**
  window covering the game (LS requires games in borderless/windowed — same
  constraint; true exclusive-fullscreen capture+overlay is not reliably
  possible). A Vulkan Win32 swapchain (option 1) or DXGI flip-model swapchain.
- **Pacing**: the existing sleep-based pacing (`swapchain.cpp` logic) to space
  real+generated frames evenly; integrate with the display refresh / VRR.

### 2.3 Frame-arrival + generation loop
- Detect each new captured frame, feed the previous+current into the flow +
  interpolation (or extrapolation) exactly as `framegen.cpp` does, emit N frames.
- Reuse `MODE`, `MULTIPLIER`, `PRESET`, `RENDER_SCALE`, `SHARPNESS`, the HUD.

### 2.4 App shell / UX (this is a whole sub-project)
- A **config window** (game/monitor picker, the same options as the GTK panel),
  a **global hotkey** to toggle generation on the focused game (the LS UX),
  a system-tray presence, per-game profiles.
- Installer (Inno Setup / MSIX), optional Authenticode **signing** (unsigned +
  injection-free is fine for SmartScreen; still worth signing for trust).

### 2.5 Config reuse
- The INI config model (`bdex-framegen.conf`, the control-panel format) can be
  reused as-is; only the process-name matching and paths change (see
  `windows-port.md` §2.2 for the `%APPDATA%` / process-name shims — those apply
  here too since we still read the same config).

---

## 3. Phased plan

**Phase 0 — decision (no code).** Confirm: external-capture, extrapolation-first,
Vulkan-compute + D3D-interop, honest about latency. If minimal latency is the
hard requirement, capture is the wrong tool and only present-hook injection (per
API, anti-cheat risk) gets there — decide now.

**Phase 1 — a proof of concept end to end (the risky 20% first). —
Implemented in `capture/` (pending on-hardware validation).**
- WGC capture of one windowed D3D11 game → D3D11 texture. ✅ `capture.cpp`:
  `GraphicsCaptureItem` from the HWND, free-threaded frame pool, a capture
  thread copying each frame into a 3-slot ring of `B8G8R8A8` textures created
  with `SHARED_NTHANDLE | SHARED_KEYEDMUTEX`; the pool is recreated when the
  window grows.
- Interop that texture into a minimal Vulkan compute context; run just
  `upscale.comp` (cheapest) and present the result in a borderless overlay. ✅
  `vkctx.cpp`: import via `VK_KHR_external_memory_win32`
  (`D3D11_TEXTURE_BIT`, dedicated allocation), keyed-mutex acquire/release
  chained on the `VkSubmitInfo` (`VK_KHR_win32_keyed_mutex`), ownership
  transfer barriers from/to `VK_QUEUE_FAMILY_EXTERNAL`, the layer's
  `upscale.comp` (embedded through the same `bdex_add_shaders` pipeline) into
  an `R32_UINT` stage image, copy into a Win32 swapchain (mailbox, or FIFO
  with `--fifo`). `main.cpp`: window picker (`--list`, `--title`, `--pid`,
  or the foreground window after 5 s), topmost no-activate overlay following
  the game window (`--scale`, `--fit`), fps HUD, Ctrl+Alt+Q.
- **Acceptance:** the overlay shows the captured game, upscaled, in real time,
  with acceptable latency. This validates capture + interop + present — the
  three genuinely new risks — before wiring the full flow pipeline.

**Phase-1 validation checklist** (first run on a real machine; the MSVC build
itself is done by the `windows` GitHub Actions workflow, artifact
`bdex-framegen-windows-x64`):
1. `bdex_capture.exe --list` prints the open windows.
2. Start a windowed D3D game; `bdex_capture.exe --title <game> --hud 2`.
   Expected log lines: `GPU: …`, `vulkan ready`, `capturing window WxH`,
   `capture ring WxH (generation 1)`, `swapchain WxH, N images, mailbox`,
   `imported ring slot 0…2`, then `capture X fps -> output Y fps` every 2 s.
3. The overlay shows the game live; `--fit` fills the monitor; `--scale 2`
   on a small window shows the bicubic upscale.
4. Resize / move / minimise the game window: overlay follows, ring
   regenerates (`generation 2`), no validation errors with
   `BDEX_CAP_VALIDATION=1` (the external-queue-family barrier layouts and
   the keyed-mutex submit are the two places a driver could disagree).
5. Note the latency subjectively (mailbox vs `--fifo`) and whether mouse
   clicks reach the game through the overlay (`WS_EX_TRANSPARENT`); both
   feed phase 3.
6. Test on both vendors available (the RTX 3060 Ti box first; AMD/Intel
   if any) — D3D11 import + keyed mutex is the cross-vendor unknown.

**Phase 2 — frame generation. — Implemented, first run validated on hardware
(2026-09-15).**
- Wire the full `framegen.cpp` flow + interpolate/extrapolate pipeline onto the
  captured frames; add multiplier, HUD, pacing, present-rate detection. ✅
  `vkctx.cpp` builds the layer's `DeviceData` over the capture app's plain
  Vulkan device and drives `FrameGen` unchanged: each captured frame is copied
  into a 3-image history ring (the role of the game's swapchain images in the
  layer), then the same sequence as `VirtualSwapchain::present` runs
  (acquire sources → analysis → flow → interpolate/extrapolate → upscale/HUD →
  release), and `present()` copies the real / generated frames into the
  overlay swapchain paced like `presentOne` (extrapolation drops a predicted
  frame when the next real one arrives early; FIFO leaves pacing to the
  display). Options: `--multiplier`, `--mode`, `--preset`, `--filter`,
  `--sharpness`, `--hud`, plus the config file / `BDEX_FG_*` environment.
- **Acceptance:** a D3D game at 30/60 fps shows 60/120 with the flow quality of
  the Linux build.

Phase-1 feedback from the first real run (2026-09-15, laptop Intel iGPU +
RTX 3050, PRAGMATA at 32 fps): capture + interop + present worked first try;
the app had picked the Intel GPU (fixed: discrete first, `--gpu`, D3D11 on the
same LUID) and the image flickered (fixed: per-submission command slots, no
descriptor/stage image shared between frames in flight).

Phase-2 first run (2026-09-15, same laptop): frame generation confirmed on
PRAGMATA (D3D12), Dark Souls III and Skyrim SE (D3D11) and Fallout: New
Vegas (D3D9) — steady x2 extrapolation at ≈2× output fps, and the pacing
degrades/recovers cleanly around game stutters. One overlay bug found and
fixed: the overlay window had its own arrow class cursor, which stayed
visible on top of games that hide the hardware cursor (double/lingering
cursor on PRAGMATA and DS3) — now no class cursor and `WM_SETCURSOR` is
swallowed, so the overlay mirrors the game's own cursor choice.

**Phase 3 — product.**
- Config window, global hotkey, per-game profiles, tray, DupliAPI fallback,
  fullscreen/borderless handling, HDR, VRR, multi-GPU.
- Installer + signing + docs.

**Phase 4 (optional) — low-latency mode.**
- Present-hook injection for D3D11/12 as an opt-in per-game mode for
  single-player titles that need minimal latency. Separate, fragile track.

---

## 4. Risks & unknowns

- **Latency** (§0) — the headline trade-off; set expectations.
- **D3D↔Vulkan interop** reliability across NVIDIA/AMD/Intel (keyed-mutex sync,
  format/tiling mismatches). Phase 1 exists to de-risk exactly this.
- **Overlay over fullscreen**: games must be borderless/windowed (LS limitation
  too); exclusive fullscreen + overlay is unreliable.
- **Capturing protected content / anti-cheat**: WGC can be blocked for some
  windows; a few anti-cheats dislike overlays. Injection mode (Phase 4) is the
  real anti-cheat risk; external capture is much safer but not zero.
- **HDR, VRR/G-Sync, mixed-refresh multi-monitor, DPI scaling** each need
  explicit handling.
- **No Windows machine in this environment** — every phase needs real hardware
  and real games to validate; this cannot be done blind.

---

## 5. Effort estimate (rough, and it is large)

This is essentially **building a Lossless Scaling-class application**, reusing
our proven frame-gen algorithm. Ballpark:

| Phase | Size |
|---|---|
| 1 — capture + interop + present PoC | ~1–2 weeks (interop is the wildcard) |
| 2 — full frame-gen on captured frames | ~1–2 weeks |
| 3 — product (UI, installer, robustness) | several weeks; UX/robustness is open-ended |
| 4 — optional injection low-latency mode | separate, per-API, weeks |

The algorithm (our advantage) is the *small* part; **capture, interop, present,
overlay, pacing, UX, and cross-vendor robustness are the large part.** This is a
multi-week-to-months product, not a port.

---

## 6. Alternatives considered / rejected

- **Vulkan layer on Windows** (`windows-port.md`): only sees native-Vulkan
  games; rejected as the primary Windows story (the user's point).
- **DXVK-on-Windows to force D3D→Vulkan so a layer sees it**: adds a translation
  layer's overhead/latency and is per-game fiddly; rejected (the user's point).
- **Present-hook injection as the primary path**: lower latency but per-API and
  anti-cheat-risky; kept only as an optional Phase-4 mode.
