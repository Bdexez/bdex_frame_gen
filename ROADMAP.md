# Roadmap

Status as of 2026-10-15. This page only says where the project stands and
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
2. **Windows layer on hardware**: it now runs on a real GPU under Wine
   (`tools/ci/wine-gpu-check.sh`: Vulkan 64/32-bit, D3D11/12 through the
   copied DXVK / VKD3D-Proton, 2026-11-10), not yet on Windows itself: the §3 checklist of
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
6. **Publishing**: 0.5.6 (2026-10-05) is a GitHub release. The AUR,
   COPR / OBS, a PPA or Flathub need a public repository and accounts; the
   packages are ready. Before the AUR: `bdex-framegen-bin`'s
   `sha256sums=SKIP` replaced by the release's `SHA256SUMS-linux.txt`.
7. **Code signing (Windows)**: an Authenticode certificate, then its `.pfx`
   (base64) and password in the repository's secrets
   `WINDOWS_SIGN_PFX_BASE64` / `WINDOWS_SIGN_PFX_PASSWORD`; `windows.yml`
   signs the layer, `bdex_capture.exe` and `bdex-calibrate.exe` on tags from
   then on (SmartScreen, antivirus wary of an .exe copying DLLs into game
   folders). Also: enable Dependabot alerts in the repository's settings
   (`.github/dependabot.yml` only schedules the update pull requests).

## Next — code

- **Quality.** Measured on 2026-10-04: a fast camera is followed (34 dB at
  80 px a frame pair). **60–80 % of the remaining error is at the edges of
  objects moving on their own**, on 2–4 % of the pixels. Sub-pixel
  refinement, bicubic synthesis, the learned flow, more pyramid levels and
  more iterations change nothing there. Other weak spots: particles
  (17.7 dB) and rotation / zoom (each block carries a translation only);
  brightness pulses are now better than the plain cross-fade (corpus
  `pulse` 35.3 dB against 28.7, 2026-10-21).
  **Measured on 2026-10-05 (`tools/edge_bench/`): it is occlusion.** Pixels
  visible in only one of the two frames hold 51–78 % of all the error; the
  flow's resolution is not the cause (`flow_scale=1` is worse). Picking the
  right one-sided sample would give up to ~+4 dB, but which layer is in
  front cannot be told reliably from two frames. **Settled on 2026-11-01**
  (`tools/edge_bench/depth_oracle.py`, MEMOIRE §7): frame N−2 alone loses
  (−0.95 dB, it mistakes uncovered pixels for covered ones), and even an
  exact per-pixel segmentation with "the faster layer in front" loses on
  fast motion (−0.44 dB); the exact depth wins on every sequence (+0.17 to
  +1.54 dB, the edge pixels only). **Implemented the same day as the opt-in
  `depth` option**: the layer finds the game's depth buffer in its
  commands (Vulkan, and DXVK / VKD3D-Proton under Proton, checked under
  Wine), reads it at each present and orders the candidate trajectories at
  the edges by it; exact depth: sharp edges +0.22 to +1.68 dB, blurred
  game-like scenes −0.05 to +0.01. First real game the same day: Oblivion
  Remastered (VKD3D-Proton, 1440p x2) reads its depth (`depth=on`), and the
  player saw fewer artifacts (an impression, not a measurement). Since then
  the status files show the depth buffer found (`depth=on`) in CK3 (native
  Vulkan), GTA IV and State of Decay 2 (DXVK) and Civilization VI
  (VKD3D-Proton); its quality is judged nowhere yet, and the panel's
  recordings keep the depth since 2026-11-11 (`depth/fNNNN.f32`, what
  `bdex_offline --depth` reads). Next: record games with it, judge it on
  them, then predicted frames (`reveal_fill`,
  the splat's priority by depth), then the game's own motion vectors (below).
- **x4 on a 30 fps game** (2026-10-13 to 21): x4 is again exactly 4 frames a
  game frame by default (the player's choice, 2026-10-20: `refresh_fill`
  off); on 144 Hz that leaves ~24 refreshes a second held whatever the
  pacing (the panel says so and names the even refresh rate: 120 Hz).
  `tight_hold` gives that refresh to the real frame (−2.8 ms), the Xwayland
  grid holds under a loaded compositor, `mosaic_blur` softens the moving
  character's mosaics (judged at a 30 fps game's real speed too:
  `tools/edge_bench/make_game.py`, the recordings accelerate the motion),
  `hud_layer` keeps HUD text and subtitles in place over the moving scene
  (and lets go at once when they disappear), `hud_avoid` keeps the icons
  apart in predicted frames too, and `hybrid_predict=1` cuts the latency
  for one short prediction (−0.67 dB at real speed). Against the version of
  20/10: game-like scenes x4 +0.14 dB, recordings +0.08 / +0.11. Still open
  there: the scene behind a see-through box (the box's area stays 5 dB
  under the picture), near objects and a first-person weapon's edges in a
  turn (occlusion), whip pans; a 120 fps recording without generation, at
  `record_scale = 1`, would give the first real-content x4 at real speed.
  Earlier: the cadence fills every
  refresh (`refresh_fill`, `virtual_grid`, `pace_plan`), and the synthesis
  lost its main visible defects on these benches — the block mosaic over
  blurred content (`false_detail`), the smeared screen edges in camera turns
  (`border_predict`), thin near structures in parallax (`parallax_search`,
  opt-in). Still open: the scene in the gaps between HUD elements (three
  fixes measured negative: do not retry), occlusions at moving objects'
  edges, mouse flicks (motion blur and 300+ px a pair), a rotating minimap,
  bloom halos. And the user's own screen: the 1440p cadence bench in
  headless gamescope cannot tell its missed refreshes from ours
  (`tools/cadence_report.py` on the real screen can).
- **Found on 2026-11-10** (detail and numbers: MEMOIRE §7, « 10/11 »):
  - fixed the same day (CHANGELOG): the refresh rate read about twice too
    high for 60 fps games under Xwayland (a sticky short period that
    "explained" completions on no grid), `vkGetDeviceImageMemoryRequirements`
    for `depth`, Steam's `fossilize_replay` armed and its status files, and
    on Windows the paced waits (9 ms late under Wine) — found by running the
    Windows layer on the GPU (`tools/ci/wine-gpu-check.sh`);
  - the GPU cost at 1440p x4: `false_detail` is ~25 % of the generation
    (1.75–1.9 ms a game frame) for a few hundredths of a dB on average; under
    saturation the low-value passes should go before the game is held back
    (`gpu_budget` skipped them: its level 1 takes them since 2026-11-11; by
    default it is off); the per-pass profile (`BDEX_FG_PROFILE=2`) exists
    since the same day;
  - the game's motion vectors (TAA / DLSS / FSR velocity buffers), found
    like the depth buffer: never considered; an `edge_bench` oracle first;
  - the validation warning under a frame limit: not the `fps_limit_wait`
    path; with validation 1.4.363 it is `WRITE-AFTER-PRESENT`, 4 runs in 10
    under X11 with a limit, none without — a minimal repro to settle it.
- **Done on 2026-11-11** (MEMOIRE §7, « 11/11 »): the game's own cadence no
  longer read as the screen's refresh rate under Xwayland (a real trace of
  a 60 fps game is a unit test); `profile = 2`, the GPU time per engine step
  and optional pass (repeatable to 0.01 ms on an idle GPU); the panel's
  recordings keep the game's depth, and `pending_decisions.py` scores
  `depth` on them; the layer reads the environment once (`getenv` against a
  game's `setenv`, found by a static-analysis pass); `bdex-x11-trace`;
  `gpu_budget`'s level 1 frees 39 % of the generation instead of 9 %.
  **Left from it**: recordings of real games made with `depth = 1` (the
  judgement itself); State of Decay 2's completions on screen
  (`bdex-x11-trace watch`: one MSC for two presents, not in the data yet);
  generation suspended while the window is hidden (its MSC stands still);
  whether the low-value passes should also go, by default, before
  `gpu_headroom` holds the game back (the player's choice).
- **Windows**: forward `VK_EXT_full_screen_exclusive` through the virtual
  swapchain; an installer with Authenticode signing; a Desktop Duplication
  fallback for capture; measuring capture latency from the game's present
  to the screen.
- **Infrastructure**: a self-hosted runner with a GPU, so the quality gate
  runs on real hardware and not only on lavapipe.
