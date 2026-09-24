# Roadmap — what's done and what remains

Status overview of **bdex-framegen** as of 2026-09-23. It summarises the
current state across the three tracks (Linux, the Windows Vulkan-layer port,
and the Windows capture product) and the algorithm-quality work. The detailed
Windows scoping lives in [`docs/windows-port.md`](docs/windows-port.md) (mode 1)
and [`docs/windows-capture.md`](docs/windows-capture.md) (mode 2); the
per-release feature log is in [`CHANGELOG.md`](CHANGELOG.md).

---

## ✅ Done

### Linux core — complete and shipping

Everything below builds and passes both test suites (unit + integration) and is
in use day to day.

- **Frame generation**: GPU optical flow (luma pyramid, hierarchical block
  matching, 4×4 refinement), bidirectional-warp synthesis, and both
  **interpolation** and **extrapolation** (`MODE=extrapolate`, ~0 ms hold).
  x2/x3/x4 multipliers.
- **Spatial upscaling** (`RENDER_SCALE` / `UPSCALE`): the display-sized-copy fix
  (no more black region) **plus** the native-**Wayland** extension, so it now
  engages on X11 / Xwayland / native Wayland. Reconstruction filters
  bilinear / bicubic / Lanczos, plus a **CAS** sharpening pass (`SHARPNESS`).
- **Proton**: works through DXVK / VKD3D-Proton (hooks
  `vkGetPhysicalDeviceSurfaceCapabilities2KHR`), presents via mailbox with its
  own pacing.
- **On-screen fps HUD** with 4 detail levels (fps, game>output, 1% low,
  0.1% low).
- **Control-panel GUI** (GTK4 / libadwaita, Lossless-Scaling style): the layer
  is always loaded and gated on the config file, so frame generation turns on
  with **no Steam launch options**.
- Per-GPU auto-tuning; 64-bit and 32-bit install scripts.

### Windows mode 1 (Vulkan-layer "injection") — Phase 1 implemented in code

Targets **native-Vulkan games and emulators** (single-player). Builds with
MSVC in Windows CI (x64 and Win32); **not yet run or validated on Windows hardware**.
See [`docs/windows-port.md`](docs/windows-port.md).

- `layer/src/platform.{h,cpp}`: process names (`GetModuleFileNameW` /
  command line), environment enumeration (`_environ`), config path under
  `%APPDATA%\bdex-framegen\`.
- Export macro `__declspec(dllexport)`; `__GNUC__` guard on the printf-format
  attribute.
- **MSVC** CMake branch (`.dll` output, no version-script / `-Bsymbolic`, `/W4`).
- PowerShell `tools/register-layer.ps1` / `unregister-layer.ps1` (per-user
  registry install under `HKCU`, no admin; writes the manifest itself).
- **Anti-cheat auto-retract**: detects EAC / BattlEye / GameGuard / XIGNCODE3 /
  miHoYo and forces the layer off for that process (also runs on Linux, catching
  the Wine/Proton case via `/proc`).

### Windows mode 2 (capture-based product) — Phases 1 and 2 implemented

`capture/` builds `bdex_capture.exe` (MSVC, x64): Windows.Graphics.Capture of
one game window → D3D11 ring textures (NT handles + keyed mutex) → imported
into Vulkan (`VK_KHR_external_memory_win32` / `VK_KHR_win32_keyed_mutex`) →
**the layer's `FrameGen` engine unchanged** (history ring, optical flow,
x2/x3/x4 interpolation or extrapolation, upscaling, HUD) → Win32 swapchain in
a topmost overlay that follows the game window, paced like the layer. Window
picker, `--scale` / `--fit`, `--multiplier`, `--mode`, `--preset`, `--gpu`.
**Phases 1 and 2 were validated on real hardware on 2026-09-15** (Intel
iGPU + RTX 3050 laptop): phase 1 on PRAGMATA (capture, interop, presentation);
phase 2's first run the same day on PRAGMATA (D3D12), Dark Souls III and
Skyrim SE (D3D11) and Fallout: New Vegas (D3D9) — steady x2 extrapolation at
≈2× output fps, pacing survives game stutters. See
[`docs/windows-capture.md`](docs/windows-capture.md) §3 and
[`docs/windows-README.md`](docs/windows-README.md) for usage.

### Windows CI

`.github/workflows/windows.yml` builds both Windows products with MSVC on
every push (x64 + Win32 matrix: layer DLL, and `bdex_capture.exe` on x64),
runs the unit tests, uploads one zip per architecture, and attaches them to a
GitHub release on `v*` tags. This is how a Windows `.exe` is produced without
a Windows machine.

---

## 🔲 Remaining

### Windows mode 1 — validation & packaging

- **No runtime validation on Windows hardware yet** (no Windows machine
  available in this environment). Windows CI builds and tests the DLL with
  MSVC; the §3 checklist in `docs/windows-port.md` — local install,
  `register-layer.ps1`, `vulkaninfo`, and a native-Vulkan title — remains.
- **Phase 2**: verify upscaling / HUD on Windows; 32-bit build +
  `WOW6432Node` registration; release zip + optional Inno/NSIS installer.
  **Forward `VK_EXT_full_screen_exclusive`** through the virtual swapchain so
  native-Vulkan games get true exclusive fullscreen (currently falls back to a
  composited present; documented in `docs/windows-README.md`).
- **Phase 3**: a Windows config tool (there is currently **no** Windows GUI —
  the GTK panel is Linux-only), Authenticode signing, a Windows section in the
  README.

### Windows mode 2 (capture-based product, multiplayer-safe)

- **Assess phase 2 quality on hardware**: the first run works (D3D9/11/12
  titles, x2 extrapolation, pacing survives stutters); still to judge: flow
  quality vs the Linux build, the low-fps regime (x2 at 30/32 fps), the x3/x4
  multipliers, interpolation vs extrapolation.
- **Phase 3** is under way: the **config window** exists (v1 launcher GUI —
  `bdex_capture.exe` with no arguments: window picker + the panel options,
  persisted to the shared config); a **global toggle hotkey** (Ctrl+Alt+G,
  rebindable via the `hotkey` config key) turns generation on/off at runtime;
  **per-game profiles** apply and can be saved to a `[game]` section from the
  launcher. Remaining: tray, Desktop Duplication fallback, HDR/VRR, installer,
  input pass-through.

### Optical-flow quality

- The weak regime is **fast motion** (~23.6 dB in the historical eval). The
  2026-09-15 fixed-display baseline was 31.0 / 23.7 dB for normal / `--speed 2`.
  Current scores vary with the window extent assigned by the compositor;
  compare variants within one display setup.
- A round of matcher A/B (2026-09-15) found the fast case is a *matching-cost
  ambiguity* problem, not range or regularisation: `levels=5` is byte-identical
  and both smoothness directions hurt. Three fixes were measured; none is a
  clean win, because everything that adds discrimination also degrades the
  static/small-motion case the current tuning nails:
  - **Gradient-constancy term** — fast +0.5 dB but normal −2.1 (the grad term
    inflates the SAD scale so the additive regularisation goes slack). Shipped
    as an **opt-in `grad_weight` knob (default 0 = unchanged, bit-for-bit)** for
    motion-heavy games; whole-frame PSNR likely understates its anti-tearing
    benefit (tearing is localised to moving edges).
  - Sub-pixel parabolic refinement: −0.1 both (synthesis already samples flow
    bilinearly). Adaptive zero-bias: only +0.16 fast (SAD-zero is a poor
    "is-moving" discriminator).
- Identified next step was: decouple the discriminative signal from the
  additive regularisation — a **census/rank-transform pyramid** — or a matcher
  that models rotation/scale. **Done (2026-09-17): the census re-ranking.**
  The census (8-bit rank transform on the 3×3, computed from the tiles already
  staged in shared memory — no extra pass or bandwidth) re-ranks the
  near-winners of an ambiguous match (runner-up within `census_margin` of the
  winner). Measured on the eval (quality preset, 2026-09-17): like the
  gradient term it is **not a clean win** — re-ranking broadly
  (`census_weight=2`, `census_margin=1`) gains **+0.45 dB fast but loses
  −1.5 dB normal**; tight gating (`margin=0.05`, `weight=0.5`) is the only
  non-negative setting (+0.12 dB normal, fast unchanged); a soft
  ambiguity-proportional weight is dominated by the hard gate. Shipped as an
  **opt-in `census_weight` knob (default 0 = bit-for-bit)**, same pattern as
  `grad_weight`. Conclusion so far: the residual fast-motion error is only
  marginally a *candidate-selection* problem; the next lever to try is on the
  synthesis side (occlusion handling in the interpolation) or a matcher that
  models rotation/scale.
- **Synthesis-side round (2026-09-20, measured, no change shipped):** four
  interpolation variants were A/B-tested on the eval harness (balanced preset,
  RTX 3060 Ti, baseline **28.91 dB normal / 23.52 dB fast**; at `quality`:
  30.25 / 23.57, close to the published 31.0 / 23.7):
  - forward/backward-consistency penalty added to each candidate's
    photometric error (the signal the extrapolation branch already uses):
    0.25 → 26.47 / 22.13; 0.1 → 28.65 / 23.44 — the inconsistency signal is
    too noisy at reduced flow resolution;
  - "best-candidate" fallback instead of the cross-fade where neither
    candidate explains the pixel: 28.30 / 22.98 (hard switching ghosts more
    than the cross-fade);
  - sharper candidate weights (1/e³ instead of 1/e²): 28.92 / 23.52 —
    byte-identical outcome, the weights already saturate to a hard pick;
  - tighter cross-fade thresholds (0.08–0.18 instead of 0.12–0.25):
    28.81 / 23.49.
  An error-map analysis of the fast case shows the residual error is
  concentrated entirely on the fast *rotating* objects, where the flow itself
  is wrong — confirming the synthesis side cannot recover it.
- **Matcher rotation/scale round (2026-09-20, shipped as opt-in
  `rot_weight`, default 0 = bit-for-bit):** the top-2 displacements of an
  ambiguous, non-static block are re-evaluated under 15 local transforms
  (3 scales × 5 angles, sampled bilinearly from the staged tile) and the
  winner switches only on a clear improvement; the flow output stays a
  translation. Measured on the eval (RTX 3060 Ti): **+0.25 dB normal, +0.32 dB
  fast (`--speed 2`), +0.11 dB at `--speed 2.5`** at `balanced`, +0.28 dB fast
  at `quality` — the first matcher change that is non-negative in *both*
  regimes (static scenes skip the re-ranking by construction). A gate on
  ambiguity is required: without it the fast case regresses (−0.2 dB). Stays
  opt-in until validated on more real titles; candidate next steps are more
  angles/scales, top-3 re-ranking, or an affine output (non-translational
  flow) so the synthesis can warp with the transform instead of only
  re-ranking with it. **Combining it with `census_weight` was measured and
  rejected (2026-09-20):** both broad (`2`/`1` → 28.52 / 23.67) and tight
  (`0.5`/`0.05` → 29.03 / 23.82) census settings are dominated by
  `rot_weight` alone (29.16 / 23.84) — the census re-ranking perturbs winners
  the rotation re-ranking then cannot repair, so the two knobs should not be
  combined.
- **Linux CI (2026-09-20, green 2026-09-21, `.github/workflows/linux.yml`):**
  every push builds the layer and demo, runs the unit + integration tests
  (under Xvfb, on lavapipe — the runners have no GPU) and gates the PSNR
  quality eval (fail below **28.0 dB normal / 23.0 dB fast** at `balanced`).
  This replaces the hand-run test suite and protects the matcher quality
  numbers against regressions. lavapipe turned out to track the GPU baseline
  to within ~0.04 dB (28.87 / 23.49 vs 28.91 / 23.52), so the gate could be
  set tight, just under the real numbers, instead of the initial loose
  27 / 22. Bring-up took three fixes worth remembering for any future runner
  work (all in the workflow): the shader compiler is the `glslc` package (not
  `glslc-tools`); lavapipe must be selected by **globbing** the ICD path and
  verifying it with `vulkaninfo` (a hardcoded `VK_ICD_FILENAMES` makes the
  loader find no driver at all on some runner images — a bare
  `vkCreateInstance` -9); and the layer must be registered in the global
  `implicit_layer.d`, because `VK_ADD_IMPLICIT_LAYER_PATH` (what the test/eval
  scripts set) is not honoured by every loader build, so the demo otherwise
  runs pass-through and the integration test fails "layer did not activate".
- **Offline evaluation (2026-09-24):** `bdex_offline` + `tools/eval_sequence.py`
  replay any recorded sequence headless and A/B options on identical inputs.
  First finding on the demo (Vega 8, x2, 40 generated frames): **extrapolation
  barely beats repeating the frame** — 23.50 dB vs 22.28 dB for plain
  repetition (normal), 19.74 vs 19.24 (fast), against 31.53 / 23.54 dB for
  interpolation. This matches the Kenshi feedback (artefacts very visible in
  extrapolation) and makes the extrapolation synthesis the next target.
- **Extrapolation double images fixed (2026-09-24):** the fallback that kept
  inconsistent pixels in place drew moving objects at their old position.
  Following the warp everywhere: +1.8 dB normal / +1.0 dB fast (online), and
  revealed areas 7.6 → 20.3 dB. With `zero_bias=0` on top (offline):
  demo 26.03 dB, fast 21.39 dB, vs 23.50 / 19.74 before — extrapolation now
  clearly beats frame repetition (22.28 / 19.24).
- **`zero_bias` is the fast-motion culprit (2026-09-24).** Its displacement
  penalty is absolute, not relative to the block's contrast, so on smooth or
  low-contrast content the free zero-motion candidate beats the true motion
  and the synthesis cross-fades. A 32 px/pair pan of a fractal texture scores
  28.5 dB by default and 46.2 dB with `zero_bias=0`; on the demo (online
  harness, 40 frames) `--speed 2` goes 23.26 → 24.01 dB (rotating edges
  +2.8 dB) for −0.10 dB at normal speed. Smoothness is not the issue (±0.0).
  Candidate default change — pending a check on recorded game sequences for
  shimmering on static low-contrast content (what the bias protects).
- `zero_bias_contrast=0.1` (opt-in, 2026-09-24): the bias scaled by the
  block's contrast. Online: normal +0.02 dB, fast +0.48 dB (rotating edges
  +2.7 dB); smooth pan +17 dB. Non-negative in both regimes — best candidate
  for a default change (pending real game sequences).
- `temporal_prediction` (opt-in): the coarsest level searches around the
  previous pair's motion. With `zero_bias=0` it follows a 64 px/pair pan
  (23.5 → 36.0 dB); alone it cannot bootstrap (+0.3 dB).
- `static_guard` (opt-in): per-pixel "unchanged in both frames → keep the
  current frame", interpolation only. HUD-over-pan scene +0.98 dB (minimap
  +4.4 dB), demo +0.01, fast −0.20 dB (a small object crossing a pixel between
  the frames is locally indistinguishable from a static glyph). Needs real
  game sequences with HUDs to decide a default.
- The first `occlusion_strength` experiment used local forward/backward flow
  consistency. On Radeon Vega 8, x2/40 frames, strength 1 matched the default
  whole-frame PSNR (28.63 dB normal, 22.04 dB fast) and reduced moving-edge
  PSNR by 0.01 dB. A local flow-divergence variant also regressed and was
  discarded. The current opt-in prototype additionally suppresses the plain
  cross-fade fallback when both warp candidates are uncertain. On the same
  GPU/display, x2/30 frames (26 aligned): normal strength 0→1 was 29.35→29.43
  dB globally, 19.97→20.16 dB on moving edges, 25.88→26.69 dB in pixels
  revealed by the moving bar, and 21.94→22.18 dB around rotating edges.
  At `--speed 2`: 22.12→22.15, 15.35→15.38, 19.62→19.95, and 13.88→13.95
  dB respectively. GPU interpolation time was noisy (roughly 7–16 ms default,
  9–15 ms opt-in on this iGPU); measure on a stable GPU before making it a
  default. At strength 0, 38 matching output dumps were byte-identical to the
  prior shader. The evaluation tool now scores changing regions, moving edges,
  revealed bar pixels, and rotating edges while excluding the demo's counter;
  it scores every position of x3/x4. Linux CI also compares default frames
  with the pre-census build byte for byte.

---

**In one line:** the **Linux product is complete and shipping**; the **Windows
mode-2 capture app runs frame generation on real hardware** (D3D9/11/12
validated, phase-2 first run done); the mode-1 layer is CI-built but never run
on Windows; the mode-2 product shell (phase 3) remains to be built.
