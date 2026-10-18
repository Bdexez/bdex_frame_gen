# Roadmap

Status as of 2026-10-05. This page only says where the project stands and
what is left. The detail lives elsewhere: how each part works, the measured
numbers and the open problems are in [`docs/MEMOIRE.md`](docs/MEMOIRE.md)
(§9 for the open problems and the experiments not to redo), and the change
log is in [`CHANGELOG.md`](CHANGELOG.md).

## Where it stands

- **Linux, Vulkan layer**: shipping. Interpolation, extrapolation and hybrid
  up to x8, adaptive multiplier, display-feedback pacing, latency limiter,
  spatial and temporal upscaling, control panel (GTK 4, text mode, English
  and French), Proton (DXVK / VKD3D-Proton, checked in CI under Wine),
  gamescope / SteamOS Game Mode, Flatpak and Snap launchers.
- **Linux, capture** (`bdex-capture`): X11 / Xwayland, and Wayland through
  the ScreenCast portal; can generate on a second GPU.
- **Windows**: the capture app (`bdex_capture.exe`) was validated on
  hardware on 2026-09-15 with D3D9, D3D11 and D3D12 games. Injection has
  been the default since 2026-10-03 (the Vulkan layer, plus DXVK /
  VKD3D-Proton for D3D games); games with an anti-cheat are captured
  instead. **The layer has never run on a real Windows machine**: it is
  built by MSVC CI and run under Wine.
- **Packaging**: archives (glibc 2.29, x86_64 + aarch64), `.deb` / `.rpm`,
  AUR PKGBUILDs, Nix flake, Flatpak extension, Decky plugin. All verified in
  containers only.

## Next — needs a player's hardware, or accounts

1. **Recordings of real games**: they decide the opt-ins still waiting for
   real content, and they are the training data the two learned networks
   lack. See [`docs/corpus-jeux.md`](docs/corpus-jeux.md);
   `tools/pending_decisions.py` gives the verdicts in one command.
2. **Windows layer on hardware**: the §3 checklist of
   [`docs/windows-port.md`](docs/windows-port.md) and
   `tools/validate-windows.ps1`, then a D3D game through the copied
   DXVK / VKD3D-Proton.
3. **Steam Deck**: the Decky plugin, synthesis cost on RDNA2 with 8 CUs,
   the 90 Hz OLED panel, HDR.
4. **Linux matrix** in [`docs/linux-validation.md`](docs/linux-validation.md):
   Flatpak and Snap Steam, SELinux, hybrid laptops. Also the open
   **KDE + NVIDIA** case: no display feedback, and Valheim throttled.
5. **Displays**: `present_feedback` on a fixed-refresh screen (the dev box is
   VRR); a real HDR / 10-bit game; Windows capture on an HDR screen;
   `async_compute` and `gpu_budget` on an AMD iGPU.
6. **A release**: `[Unreleased]` in the changelog holds everything since
   0.5.5 (2026-09-26). Publishing on the AUR, COPR / OBS, a PPA or Flathub
   needs a public repository and accounts; the packages are ready.

## Next — code

- **Quality.** Measured on 2026-10-04: a fast camera is followed (34 dB at
  80 px a frame pair). **60–80 % of the remaining error is at the edges of
  objects moving on their own**, on 2–4 % of the pixels. Sub-pixel
  refinement, bicubic synthesis, the learned flow, more pyramid levels and
  more iterations change nothing there. Other weak spots: particles
  (17.7 dB), brightness pulses (worse than the plain cross-fade), and
  rotation / zoom (each block carries a translation only).
- **Windows**: forward `VK_EXT_full_screen_exclusive` through the virtual
  swapchain; an installer with Authenticode signing; a Desktop Duplication
  fallback for capture; measuring capture latency from the game's present
  to the screen.
- **Infrastructure**: a self-hosted runner with a GPU, so the quality gate
  runs on real hardware and not only on lavapipe.
