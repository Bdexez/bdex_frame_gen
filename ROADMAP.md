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
- **No CI at all** yet (no `.github/workflows/`, neither Linux nor Windows). A
  Windows MSVC build job would de-risk Phase 1 without needing a physical box.

### Windows mode 2 (capture-based product, multiplayer-safe)

- [`docs/windows-capture.md`](docs/windows-capture.md) is **scoping only — no
  code.** It is a separate product (WGC/Desktop-Duplication capture, Lossless-
  Scaling style, render-API-agnostic across D3D9–12 / OpenGL / Vulkan) aimed at
  multiplayer, where not injecting avoids anti-cheat flags. Entire product still
  to build; the estimated effort is large.

### Optical-flow quality

- The weak regime is **fast motion** (~23.6 dB in the eval). Identified next
  step (not started): a more discriminative matching cost, or a matcher that
  models rotation/scale. Prior A/B experiments and rejections are recorded in
  the session notes.

---

**In one line:** the **Linux product is complete and shipping**; the **Windows
mode-1 port is coded but never built or run on Windows** (the concrete next step,
which needs a Windows machine); the **mode-2 capture product is entirely
unstarted**.
