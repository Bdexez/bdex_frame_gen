# Roadmap — what's done and what remains

Status overview of **bdex-framegen** as of 2026-09-15. It summarises the
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

Targets **native-Vulkan games and emulators** (single-player). Written and
compiled on Linux; **not yet built or validated on real Windows hardware**.
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

### Windows mode 2 (capture-based product) — Phase 1 PoC implemented in code

`capture/` builds `bdex_capture.exe` (MSVC, x64): Windows.Graphics.Capture of
one game window → D3D11 ring textures (NT handles + keyed mutex) → imported
into Vulkan (`VK_KHR_external_memory_win32` / `VK_KHR_win32_keyed_mutex`) →
the layer's `upscale.comp` → Win32 swapchain in a topmost overlay that follows
the game window. Window picker, `--scale` / `--fit`, filter choice, fps HUD.
Same caveat as mode 1: **compiled by CI only, never run on Windows**. See
[`docs/windows-capture.md`](docs/windows-capture.md) §3 for the validation
checklist and [`docs/windows-README.md`](docs/windows-README.md) for usage.

### Windows CI

`.github/workflows/windows.yml` builds both Windows products with MSVC on
every push (x64 + Win32 matrix: layer DLL, and `bdex_capture.exe` on x64),
runs the unit tests, uploads one zip per architecture, and attaches them to a
GitHub release on `v*` tags. This is how a Windows `.exe` is produced without
a Windows machine.

---

## 🔲 Remaining

### Windows mode 1 — validation & packaging

- **No build or validation on real Windows hardware yet** (no Windows machine
  available in this environment). The whole §3 checklist in `docs/windows-port.md`
  — MSVC build, `register-layer.ps1`, `vulkaninfo`, running a native-Vulkan
  title — is still to be exercised.
- **Phase 2**: verify upscaling / HUD on Windows; 32-bit build +
  `WOW6432Node` registration; release zip + optional Inno/NSIS installer.
- **Phase 3**: a Windows config tool (there is currently **no** Windows GUI —
  the GTK panel is Linux-only), Authenticode signing, a Windows section in the
  README.
- No **Linux** CI yet (unit + integration tests still run by hand).

### Windows mode 2 (capture-based product, multiplayer-safe)

- **Phase 1 (PoC) is coded** in `capture/` — see the Done section — but, like
  mode 1, **never run on Windows**. The phase-1 checklist in
  [`docs/windows-capture.md`](docs/windows-capture.md) §3 is the next step.
- **Phase 2** (the actual frame generation on captured frames: port the
  `framegen.cpp` flow + interpolate/extrapolate orchestration onto the
  imported textures, multiplier, pacing, present-rate detection) and
  **phase 3** (config window, hotkey, tray, per-game profiles, Desktop
  Duplication fallback, HDR/VRR, installer) are untouched. The product
  remains a multi-week effort; only the three risky unknowns (capture,
  interop, present) have code.

### Optical-flow quality

- The weak regime is **fast motion** (~23.6 dB in the eval). Identified next
  step (not started): a more discriminative matching cost, or a matcher that
  models rotation/scale. Prior A/B experiments and rejections are recorded in
  the session notes.

---

**In one line:** the **Linux product is complete and shipping**; the **Windows
mode-1 layer and the mode-2 capture PoC are both coded and built by CI, but
never run on Windows** (the concrete next step, which needs a Windows machine);
mode-2 frame generation and its product shell remain to be built.
