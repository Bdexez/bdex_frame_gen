# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### x4 on 30 fps: every refresh a new frame; frames on curved trajectories (2026-10-14)

- **`refresh_fill`** (on from x3): a fixed multiplier whose output falls
  short of a fixed refresh rate by less than a frame per game frame — 30 fps
  x4 on 144 Hz gives 120 frames for 144 refreshes, a frame held two
  refreshes every fifth, 24 times a second — now gives each game frame `m`
  or `m+1` frames, one per refresh. Headless gamescope at 144 Hz, the demo
  at 30 fps x4: 144.0 frames a second shown instead of 120, repeats 26/s →
  0.4/s; the real frame on screen ~6 ms later (31 → 38 ms present to
  screen; 40 fps x3 +5 ms). `refresh_fill = 2` also at x2 (60 fps x2 on
  144 Hz: +7 ms), `0` off; « Remplir chaque rafraîchissement » in the panel
  and the Windows launcher.
- **`virtual_grid`**: where the screen's refresh grid is not measured
  (Wayland compositors such as Hyprland, KDE with NVIDIA), the adaptive
  pacing puts its frames on a grid of the output period at a phase of its
  own, moved half a period when frames get replaced. Same demo with the grid
  hidden: repeats 29/s → 2/s. Status `display_grid=2`.
- **`pace_plan`**: on a refresh grid, each frame of a present is planned for
  its refresh when the game presents (from a cautious estimate of when its
  first frame is ready) and presented for it, so its content is that
  refresh's. The layer used to predict from the present call, the GPU still
  working: at 30 fps x4 on 144 Hz, 35 % of the frames showed a refresh later
  than their content's, irregularly — 5 % now (+1.4 ms).
- **`interp_accel`** (on on a discrete GPU): where the motion changed since
  the previous pair (a mouse turn speeding up or slowing down), the
  generated frames follow the curved trajectory (`a·t(1−t)/2` off the
  straight line), from the previous pair's flow, averaged over ~80 pixels
  and kept only where the neighbours agree. Recorded games +0.03 to +0.17
  dB, flicker −0.04 to −0.51, fewer grossly wrong pixels on every one;
  constant-speed scenes within −0.02 dB; +0.1 ms a game frame at 1440p x4.
- `latency_log` gains a 4th column: the refresh each frame was planned for.
- `global_motion` searches a third of the coarsest level, up to 24 pixels
  (was a quarter, up to 16): fast turns in the recorded games, grossly wrong
  pixels −1 to −5 %, flicker down on every one; free in GPU time.
- **A static HUD over a camera turn** came out smeared and doubled at x4 on
  30 fps: `hud_mask = 8` (on on a discrete GPU, any preset but
  `performance`) keeps pixels unchanged for 8 frames as they are —
  game-like scenes (`tools/edge_bench/make_game.py`: a 30 fps game seen at
  120 Hz, parallax, motion blur, HUD) +0.21 to +0.47 dB, edges +0.9 to
  +2.0; and **`static_soft`** (also on) leans the anti-aliased HUD text the
  hard mask misses toward its place: +0.05 to +0.25 dB, edges +0.5 to +4.5.

### Interpolation latency; the frame cap at the game's input point (2026-10-13)

- **The first generated frame no longer waits for the others.** At x3 and
  above (two queues, the default), the analysis, the flow and the first
  synthesis go in a submission of their own; the display schedule starts
  when that one is done instead of when every synthesis is. Demo at 30 fps,
  1440p, `quality`, three runs: real frame on screen x4 ~36 → ~32 ms, x3
  ~34 → ~30 ms (medians), p95 down too.
- **`fps_limit` at the game's Reflex sleep / Anti-Lag input stage** when the
  game uses them: held there, before it reads its input, as by its own
  limiter (demo with Anti-Lag: 6.6 ms input to screen at x1, the game's own
  cap 6.7). Otherwise at the end of the present, as before.
- **Status / panel**: `cap_point` (input or present) and `engine`. A
  software cap on a Direct3D game under Proton (DXVK, VKD3D-Proton) holds the
  thread they present from, while the game queues one or two frames ahead
  (its maximum frame latency): the panel says the game's own cap (or Reflex
  / Anti-Lag in the game) responds faster.
- Demo: `--anti-lag` (VK_AMD_anti_lag as a game uses it); `latency_bench.sh
  --demo-args`.

### Frame-rate cap without added latency; fast camera turns followed (2026-10-13)

- **`fps_limit` no longer delays the frame.** The layer held the game's
  present until the next slot, so every finished frame waited for it:
  +23 ms from input to screen at a 40 fps cap, x1 and x2 alike, against the
  game's own limiter. It now hands the frame on at once and holds the game
  before its next frame (where it reads its input), like a game's own
  limiter: demo uncapped and capped at 40 fps by the layer, input to screen
  30.2 → 4.5 ms at x1 (the game's own cap: 6.7), real frame at x2 43.3 →
  19.9 ms (20.2); 40.0 fps held. `latency_probe` checks it.
- **Fast camera turns** (recorded games: 150–190 px a frame at 853×480, a
  mouse flick) went past the motion search's reach and the picture broke
  up. New `global_motion` (on by default on a discrete GPU): the frame's
  dominant shift, by exhaustive search at the coarsest pyramid level, is a
  second search centre for each coarsest block. And the `quality` preset
  computes the motion at full resolution only up to ~1080p, at half
  resolution above (at 1440p the full-resolution pyramid reached a third as
  far). Recorded games (PRAGMATA, Oblivion Remastered) at 1440p, fast
  turns: grossly wrong pixels −23 % and −29 %, +1.0 / +0.5 dB; at 853×480
  −1 to −13 %; synthetic scenes within ±0.04 dB; the flow's GPU time at
  1440p 6.9 → ~2 ms a game frame.
- `tools/latency_report.py`: `BDEX_LATENCY_JOIN_MS` widens the join of the
  demo's and the layer's present stamps (to see a limiter that makes the
  call wait).

### x3 / x4: the presets keep the discrete-GPU quality options; screen timing shown (2026-10-13)

- **A preset picked by hand no longer drops the quality options of a
  discrete GPU.** `pixel_vectors`, `interp_splat`, `rot_weight`,
  `temporal_prediction`, `zero_bias=0`, `scene_cut_local` and the
  extrapolation splat were only turned on with the automatic preset: « Qualité »
  in the panel ran without them. Measured on a recorded game (PRAGMATA, RTX
  3060 Ti), `quality` with them: 17 % fewer grossly wrong pixels at x2 and
  20 % at x3 (a pixel no truth pixel within ±2 px matches), +0.06 / +0.29 dB;
  game-like edge sequences at x4 +0.4 to +1.1 dB. About +1 ms of synthesis
  per game frame at 1440p x4. `performance` keeps none of them (chosen to
  spend less); every option can still be forced either way.
- **Screen timing in the status**: `display_grid` (refresh grid measured from
  the present feedback, or not) and `fps_limit`. Without a grid, at x3 and
  above the generated frames are paced on the game's interval and land up to
  a refresh away from the moment they show (simulated, 30 fps on 144 Hz:
  −2.5 dB and twice the flicker at x2, x3 and x4 alike): the panel, the text
  mode and the Windows launcher say so and suggest the game frame-rate cap on
  Auto, or a screen rate that is an exact multiple of the game's.
- Tests: `corpus` and `offline` no longer read the user's own settings file
  (`BDEX_FG_CONFIG=/dev/null`); on a discrete GPU, `corpus` and `min_limits`
  turn the discrete-GPU options off to compare with the lavapipe references.

### Game frame-rate cap in the panel (2026-10-13)

- « Limite d'images du jeu » moves from the advanced settings to the main
  « Génération d'images » group: none, Auto (screen refresh ÷ multiplier) or
  30–120 fps, applied live to the running game (`fps_limit`, the layer's
  limiter, before generation). The row shows the caps that fill the screen
  exactly (144 Hz: ×2 → 72, ×3 → 48, ×4 → 36). Text mode: `l`.

### Control panel: detection after a reinstall, narrow window (2026-10-13)

- A game still running a layer that was reinstalled since is detected again:
  its mapped library is a deleted file, which the Steam container shows as
  `/ (deleted)`, so the panel also takes a game whose layer wrote its status
  file in the last 10 seconds. Its row says to restart the game (the new
  layer, recording included, loads at its next start) and its « ● Enregistrer »
  is greyed out: an older layer may not record, the request waited in vain.
- « Jeux détectés » in a narrow window: only « Actualiser la liste » stays
  in the header (three buttons there squeezed the title and description into
  one letter per line); the capture buttons get their own row below the games.

### Security and licensing review (2026-10-13)

The usual web-application checklist, applied where a local program has an
equivalent (`SECURITY.md` keeps the result):

- **Control panel: names shown as plain text.** libadwaita reads row titles,
  subtitles and toasts as Pango markup: a game, window or GPU name with `&`
  or `<` (« Command & Conquer ») made the row's text vanish. Plain text now,
  in both the libadwaita and the plain GTK 4 panel (which showed `&lt;` in
  some help texts).
- **No shared `/tmp` directory taken blindly**: without `HOME`, the status
  directory `/tmp/bdex-framegen-<uid>` is used only if it is ours (0700,
  owned by us); another user could otherwise plant a session file or
  symbolic links there. Layer and panel.
- **Windows, « Retirer les injections »** deletes only the translation DLLs
  it knows by name: the list it reads sits in the game's folder, and an
  edited line could have deleted any file.
- **Diagnostic reports** (`bdex-framegen --report`, the Windows launcher's)
  hide the home / profile path (`~`, `%USERPROFILE%`) and list files with
  numeric owners: they are meant for public issues.
- **LGPL compliance for VKD3D-Proton**: the Windows package now carries the
  license texts of DXVK and VKD3D-Proton (fetched pinned by SHA256), and
  each release gets VKD3D-Proton's complete source, submodules included.
- **Releases**: `SHA256SUMS-linux.txt` and `SHA256SUMS-windows.txt`;
  Authenticode signing of the Windows binaries ready in CI, active once a
  certificate is in the repository's secrets.
- **CI**: read-only token by default (only the publishing jobs write),
  third-party actions pinned by commit, Dependabot monthly (actions, Decky
  plugin), `pnpm audit` of what the Decky plugin ships, gitleaks over the
  whole history at every push (checked clean: 305 commits).

## [0.5.6] - 2026-10-05

Everything since 0.5.5, released together. **From this version on the
software is proprietary** (`LICENSE`; up to 0.5.5 it was MIT).

### Removed: the rejected experiments

- **`alt_vectors`, `affine_flow`, `flow_upsample`, `confidence_fallback`
  and `extrapolate_hold` are gone** (option, shader code, pass, panel and
  launcher entries). Each was measured negative or neutral and left off
  (`docs/MEMOIRE.md` §9 keeps the measurements); the code still cost
  shader variants, determinism and validation runs and panel surface. The
  matcher no longer writes a second hypothesis buffer, the synthesis binds
  two storage buffers less, extrapolation no longer fetches the forward
  flow for the old hold. Every other setting gives the same frames bit for
  bit (276 settings × sequences × modes compared, RTX 3060 Ti). A
  configuration file still naming one of them is warned about and ignored.

### Windows: injection by default

- **Injection is the default on Windows** (`method = auto`): the launcher
  registers the Vulkan layer for the user (HKCU, the 32-bit one too in the
  *complete* package) and arms Vulkan games. A Vulkan game is injected into
  at once; a Direct3D 9/10/11 game gets DXVK 2.5.3 next to its executable,
  a Direct3D 12 one VKD3D-Proton 3.0.1 with DXVK's dxgi — injected into
  from its next start, captured meanwhile. Nothing already next to the game
  is replaced; « Retirer les injections » removes them all. OpenGL: capture.
- **Games with an anti-cheat: capture by default** (Easy Anti-Cheat,
  BattlEye — its files beside the game, its modules, its process — and the
  competitive titles), **except Rocket League**, where injection stays on
  for now (`platformAntiCheatException`, Windows only: the layer stays on
  there as with `ignore_anticheat`). **A warning comes up before
  generation starts on any game with an anti-cheat** — launcher, Linux
  panel (dialog) and text mode — and the launch goes ahead. The layer
  reports an anti-cheat it runs beside (`anticheat=` in the status file,
  « ⚠ Anti-triche détecté » in the status lines and the tray).
- The Windows package carries DXVK and VKD3D-Proton
  (`tools/fetch_translation_layers.py`, pinned, SHA256 checked); a new CI
  artifact, `bdex-framegen-windows-complete`, adds the 32-bit layer to the
  x64 package.

### Reports

- **« Signaler un problème » in the Windows launcher**, as the Linux panel
  has: the diagnostic report (version, Windows build, GPUs, the layer's
  registration, the games with an injection, what runs now, the settings,
  the end of `log_file`) written to
  `%LOCALAPPDATA%\bdex-framegen\bdex-framegen-report.txt`, copied to the
  clipboard and shown in Explorer, then the pre-filled issue form (the same
  fields as `bdex-framegen --issue-url`).

### Quality (experimental)

- **`affine_flow`** (off): with `pixel_vectors`, the affine motion fitted
  on the 3×3 flow texels around the source tried per pixel (rotation,
  zoom), in a synthesis variant of its own (`SYNTH_AFFINE`). Measured
  neutral on the corpus — rotation and zoom are already followed by
  `rot_weight` and `pixel_vectors` (45.23 / 51.34 dB either way) —: kept
  as an option for real-game recordings to decide.

- **`learned_flow`** (off): a small network (23 → 16 → 2, `tools/train_flow.py`,
  numpy) corrects each vector of the finest flow by at most 1.5 level
  pixels, from its neighbourhood, the matching error and its slope, and its
  fractional part (`learned_flow.comp`, two stages: corrections, then
  added). Trained on synthetic motions with their true flow
  (`bdex_offline --dump-flow` exports the flow and the level-0 lumas):
  held-out end-point error 0.52 → 0.21 level px. On the corpus, never
  trained on: zoom +1.0 to +2.6 dB, sub-pixel +0.5 to +2.2, fade +1.4, the
  demo ±0.03, rotation −0.13 (interpolation); +0.3 ms a generated frame at
  1440p. Off until trained on real games' recordings.

### Wayland capture

- **`bdex-capture --wayland`**: native Wayland windows through the
  screen-cast portal (GDBus: CreateSession, SelectSources with the choice
  kept — `restore_token` cached —, Start, OpenPipeWireRemote) and a
  PipeWire stream (BGRx / RGBx, mapped memory, the window's crop region),
  copied into the same page-aligned ring as X11's — the presenter imports
  it in place. The overlay covers `--rect`, else the whole screen.
  `--screen` shares a screen. Built when libpipewire-0.3 and GIO are there.
  Panel: *Fenêtre Wayland (portail)…*, text mode `y`. CI:
  `wayland-capture-check` (sway headless with two outputs, the portal's wlr
  backend, PipeWire, lavapipe): 30 → 60 fps, x2.00, in place. Pitfall:
  offer `maxFramerate` — at 0/0 the portal's wlr backend sends one frame.

### Display timing

- **`VK_EXT_present_timing`** (`present_timing`, default 1, with
  `present_feedback`): where the driver has it (with `VK_KHR_present_id2`
  and `VK_KHR_calibrated_timestamps`; NVIDIA's does), the layer enables it
  for its own swapchains — hidden from the game like the other present
  extensions —, creates the real swapchain with it where the surface
  supports it, takes the swapchain's refresh duration as the display
  clock's hint and, where the surface reports the first pixel visible (or
  out), each frame's display time from the driver instead of the moment
  the present-wait thread woke up. A full timing queue falls back to
  present waits. Under headless gamescope (X11 surface): accepted, no
  refresh duration nor display stage — nothing changes there; to be seen
  on a real screen. The demo enables `VK_KHR_get_surface_capabilities2`
  (as DXVK / VKD3D-Proton do), which the surface query needs.

### Online benchmark

- **`bdex_demo --replay DIR [--replay-step N]`**: a recorded sequence
  played as the game's frames (frame start + k·N on present k, blitted to
  the window), so the whole online path runs on real content.
- **`tools/replay_bench.py`**: the replay through the layer, every
  presented frame dumped and scored against the frames left out, with the
  repeat and cross-fade references; identical to the offline engine on the
  corpus (pan 47.29 dB, demo 23.36, occlusions 34.20 at x2; extrapolation
  pan 43.36; hybrid x3 40.29). `--other-layer NAME` measures another frame
  generation layer on the same frames (above ours, which only records; its
  real frames anchor the generated ones). Test `replay_bench`.

### Calibration

- **`bdex-calibrate`** (Linux and Windows, installed beside the layer / in
  the Windows package): the engine on synthetic moving frames at the
  screen's size, for the automatic preset and each fixed one — analysis +
  flow per game frame, synthesis per generated frame (GPU timestamps,
  `FrameGen::profileTake`) — and the recommendation: the automatic preset
  when it fits, else balanced, else performance, and the highest multiplier
  within a quarter of the GPU's time with no game under 30 fps. Under a
  second. RTX 3060 Ti, 1440p: auto 0.43 ms + 0.55 ms a generated frame, x4
  at 144 Hz (1.8 ms a game frame).
- Run by itself at the first start of the Linux panel and of the Windows
  launcher (*Calibration du GPU*: *Mesurer*, *Appliquer*), text mode `k`,
  `bdex-framegen --calibrate [--apply]`. The panel's *Auto* multiplier
  never exceeds the calibrated one.

### Recording for the corpus

- **« Enregistrer » on a running game**: the Linux panel (a button on each
  game the layer has; text mode `e`) and the Windows tray during an
  injection ask the layer for the game's next 150 real frames
  (`<status dir>/<pid>.record`, taken by `DeviceData::pollRecordRequest`):
  written as `fNNNN.ppm` at most 960 pixels wide (box filter) while
  generation goes on, then `DONE`. The panel moves them to
  `~/.cache/bdex-framegen/recordings` and packs them with
  `tools/corpus_pack.py` when it and Pillow are there (Windows:
  `%LOCALAPPDATA%\bdex-framegen\recordings`). Test `record_live`.

### Battery

- **`battery_saver`** (default 1): on battery, the automatic preset takes
  the performance flow whatever the GPU (the flow is most of the
  generation's GPU time, so of its power); `2` also caps the multiplier at
  x2. Plugging in or out switches within 5 s. Never on a Steam Deck.
  Linux `/sys/class/power_supply`, Windows `GetSystemPowerStatus`.

### License

- **The software is now proprietary**, all rights reserved (`LICENSE`);
  versions up to 0.5.5 were published under the MIT License and stay so
  for those who have them. Third-party components: `THIRD_PARTY_NOTICES.md`.

### Injection or capture, and the GPU that generates

- **Injection where possible, capture otherwise**, on both systems: the
  Vulkan layer inside the game's own presentation adds no frame of latency,
  so the Linux control panel and the Windows launcher use it wherever it has
  the game's swapchain (a Vulkan game, Proton through DXVK / VKD3D-Proton),
  and capture the window otherwise — a D3D / OpenGL game on Windows, a game
  the layer stays out of (anti-cheat), or another GPU than the game's.
  New config key `method` (`auto`, `layer`, `capture`); each detected game
  in the panel says which and why. The panel starts and stops
  `bdex-capture` itself (« Capturer une autre fenêtre… » for a program the
  layer does not see); the launcher drives the layer through the same
  session file as the panel (`app.session`, heartbeat every second), and
  arms Vulkan games (`enabled`, `require_app`) when the layer is registered.
  `bdex_capture.exe --method auto|layer|capture`.
- **The GPU that generates** (`gpu`, a part of its name; panel « GPU de
  génération », launcher « GPU de génération », `--gpu <n|name>`): on a
  two-GPU machine, the frames can be generated on the other GPU than the
  game's — by capture, the game keeps rendering on its own.
- The capture apps read their settings as explicitly enabled: under the
  panel's arming (`require_app = 1`) they were paused, HUD off, like an
  untargeted game (`capture_linux` checks it).
- `hotkey` is a config key on Linux too (bdex-capture's toggle).

### Performance

- **The synthesis compiled for the options in use**: `interpolate.comp`'s
  opt-ins are each a bit of a specialization constant, and the engine binds a
  pipeline built for exactly the set the frame uses (pre-built with the other
  pipelines; kept in the pipeline cache) instead of the one with every
  opt-in compiled in — whose registers cost the occupancy of every pixel.
  Discrete GPU defaults (pixel_vectors, interp_splat, local scene cut):
  1440p x2 0.99 → 0.66 ms, 4K x2 2.14 → 1.43 ms per generated frame — the
  whole generation at 1440p 1.42 → 1.09 ms. Same frames (the lean and full
  variants unchanged; a specialised one within 1 LSB on NVIDIA, a few
  pixels of the HUD scene). `reveal_fill`, an extrapolation option, no
  longer keeps the interpolated frames off the lean variant.
- **`interp_splat` at half resolution**: one pixel per 2x2 block, both
  frames in one dispatch, onto a half-resolution map (a quarter of the fill):
  0.29 → 0.11 ms at 1440p, same quality (within ±0.02 dB of the
  full-resolution splat on the corpus at 960x540, `PRESET=auto`).
  With the two items above, the whole generation on a discrete GPU's
  defaults (RTX 3060 Ti, x2): **1440p 1.63 → 0.92 ms, 4K 2.96 → 1.84 ms**.
- **`synth_bicubic` 16 % cheaper** (1440p: 1.36 → 1.14 ms a synthesis): the
  trajectories are chosen on bilinear samples and only the four colours
  blended are read with Catmull-Rom. Same quality on the corpus (±0.05 dB;
  with `subpixel_lk`, sub-pixel motion +1.4 dB as before). Still opt-in: it
  gains next to nothing elsewhere for +0.65 ms at 1440p.
- **The automatic quality options up to 4K ×3** (`pixel_vectors`,
  `interp_splat`, the local scene cut on a discrete GPU; 1440p ×5, 1080p ×8):
  they stopped at 4K ×2, whose cost (2.9 ms a game frame) is now 4K ×3's.
- **Synthesis 20 % cheaper on discrete GPUs** (1440p x2, RTX 3060 Ti:
  1.20 → 0.95 ms; the whole generation 1.63 → 1.38 ms), same frames bit for
  bit: the splat map of `interp_splat` / `extrapolate_splat` is a storage
  buffer (cleared by `vkCmdFillBuffer`, buffer atomics) instead of an r32ui
  image (its clear alone took 0.18 ms, its image atomics as much), and the
  synthesis tests the error that decides whether a splatted candidate can be
  kept before evaluating it (4 samples a pixel saved where the trajectory
  found is already good — most of a frame).

### Quality

- **`zero_bias = 0` by default on discrete GPUs** (automatic preset): the
  matcher's free bonus for "no motion" made soft content in fast motion
  blend into a ghost; a static HUD still matches exactly at zero motion.
  960x540 corpus, on top of the other automatic options: interpolation x2
  zoom +1.57 dB, pan +0.45, swing +0.31, demo +0.08 / +0.26; hybrid x3 zoom
  +1.71, pan +1.51, rotation +0.82, popup +0.49; extrapolation zoom +2.16,
  pan +1.57, flash +0.50, demo +0.26. Losses: occlusions −0.11 (x2) / −0.30
  (extrapolation), HUD −0.13 (hybrid), sub-pixel −0.06. `zero_bias` set in
  the configuration is kept as is.
- **`temporal_prediction` on by default on discrete GPUs** (automatic
  preset): the coarsest matching level searches around the previous pair's
  motion. No GPU cost; on the 960x540 corpus and demo recordings, with the
  other automatic options: interpolation x2 occlusions +3.7 dB, zoom +4.4,
  fade +5.4, swing +1.7, demo +0.15 / +0.19; extrapolation occlusions +0.8,
  zoom +3.9; hybrid x3 HUD +9.8, pan +11.7, menu +5.3. Losses up to −0.4 dB
  (rotation, a menu, a swinging camera in hybrid).
- **`rot_weight` on by default on discrete GPUs, in a matcher variant of its
  own**: a second specialization constant (`kRot`) builds the lean matcher
  plus the rotation re-ranking's shared memory (~13.5 KB instead of the full
  variant's ~21 KB, which halved the occupancy). Flow 2.4x → 1.5x the lean
  matcher's time. On top of pixel_vectors: occlude +1.17 dB, 8°/frame
  rotation +1.09, 5 % zoom +1.12, scroll +0.40, subpixel +0.23, demo +0.07
  to +0.12, nothing lost. Corpus: a `zoom` sequence (2 %/frame).
- **`interp_splat`** (on by default on discrete GPUs): softmax-style
  splatting for interpolation. The extrapolation's splat pass also pushes
  every pixel of prev (forward flow) and of cur (backward flow) to the
  output's time, into the two halves of a map twice the display's width
  (extrapolation keeps the left half: unchanged); the synthesis tries the
  trajectories of the pixels that landed on each output pixel where its own
  is imperfect (error > 0.02) and the splatted one 10 % better. Alone:
  occlude +0.55 dB, scroll +0.62, rotate +0.26, popup +0.09, demo +0.05 to
  +0.08, nothing lost; on top of pixel_vectors occlude +0.22, scroll +0.41,
  popup +0.06, demo +0.01 to +0.03, hud −0.12. +0.09 ms at 960x540.
- **`luma_match`** (default 0.01): fades and lighting changes no longer break
  the motion. Before the flow, a pass brings the previous frame's luma
  pyramid to the current frame's mean and contrast (`luma_match.comp`, all on
  the GPU) when the whole picture's brightness moved — at least 13 of 4x4
  tiles the same way (a menu opening over a quarter of the screen moved the
  mean too: normalising it cost popup 1.8 dB, now 0.00). Corpus: pulse 27.0 →
  35.2 dB, fade +6.2 (extrapolation +5.7 / +1.2); every other sequence and
  the demo identical. +0.04 ms at 960x540.

### Quality (experimental)

- **Learned correction for extrapolation**: `tools/train_correction.py --mode
  extrapolate` trains a second set of weights
  (`learned_correction_extrap_weights.glsl`, `LCX_`) whose fallback is the
  current frame (nothing safer to show past it); the shader picks them for
  t > 1. Unseen sequences: demo +0.26 / +0.12 / +0.15 dB, popup +0.32, swing
  +0.07, subpixel −0.38, scroll −3.07: still off by default. Also measured
  for extrapolation and not kept: damping the prediction (demo −0.12 to
  −0.49, pan −9.2) and `extrapolate_accel` as a default (swing +2.6 but pan
  −0.78, occlude −1.05, scroll −1.40).
- **`alt_vectors`** (opt-in): the matcher's second hypothesis per block
  (lowest cost 2 px or more from the winner, `altOut`, full variant only)
  tried per pixel by the synthesis (storage buffers: the matcher keeps to
  the four storage images Vulkan guarantees — caught by `min_limits`) where
  the kept trajectory is poor and only when clearly better. Without that
  test demo +0.2 to +0.3 dB, hud +0.6, but scroll −2.2, subpixel −2.7; with
  it alone hud +0.57, pan −0.62, scroll −1.74, and on top of pixel_vectors
  nothing to gain (pan −0.57): off.
- **`flow_upsample`** (opt-in): joint bilateral upsampling of the finest flow
  to the luma resolution at motion boundaries (`flow_upsample.comp`, its own
  synthesis sets: `FgContext::synthesisSetWithFlow`). Without pixel_vectors:
  rotate +0.51 dB, hud +0.15, occlusion edges +0.99, scroll −0.88, demo
  −0.06; with pixel_vectors (on by default on discrete GPUs) the reverse
  (rotate −1.41, scroll +1.66). A floor on the brightness weight and a
  boundary-only rule were tried; no setting wins everywhere: off.
- **Six hard sequences in the corpus** (`tools/make_corpus.py`): `scroll`
  (thin text scrolling), `particles` (rain), `smoke` (a transparent layer
  over a background moving the other way), `rotate`, `flash` (one bright
  frame between inputs), `pulse` (brightness oscillating 15 %). First scores
  (49 frames, 480x272, PSNR / cross-fade): scroll 32.1 / 22.7, particles
  17.7 / 17.1, smoke 31.9 / 31.9, rotate 39.7 / 29.3, flash 39.3 / 23.9,
  pulse 27.0 / 29.2 — under changing brightness the generated frame is worse
  than a plain cross-fade. The corpus references (`tests/corpus_reference.json`)
  refreshed from lavapipe: the old ones predated September's gains (pan
  interpolation 43.4 against 46.5 now) and caught little.
- **Learned correction** (`learned_correction`, opt-in): a network of 63
  inputs (the 3x3 neighbourhood of the generated colour, of the inputs'
  cross-fade and of their luma difference), 8 ReLU units and one output — a
  bounded weight that moves the pixel toward the cross-fade, never past it
  (a free colour residual made correct pixels worse: subpixel −11 dB).
  Trained offline with numpy by `tools/train_correction.py` from bdex_offline
  runs (weights in `layer/shaders/learned_correction_weights.glsl`, record in
  `layer/learned_correction.json`), run over each finished generated frame
  before the HUD (`learned_correction.comp`, 8-bit, no upscaling). On
  sequences never trained on: demo +0.49 dB, other demo runs +0.10 / +0.15,
  swing −0.03, popup −0.59, subpixel −1.73; the engine's results match the
  numpy model's. +0.12 ms at 960x540. Off by default: it needs recordings of
  real games to generalise.

### Diagnostics

- **GPU memory held by the layer** in the "frame generation ready" log line
  (every image and buffer of its engines on the device). Measured (dGPU
  defaults): 1440p x2 75 MiB, x4 105 MiB; 4K x2 165 MiB, x4 229 MiB.
- **Parser fuzzing** (`tests/fuzz/fuzz.cpp`, `tools/ci/fuzz.sh`, CI job
  `fuzz`): libFuzzer with ASan and UBSan on what the layer reads from files —
  the configuration and the shipped profiles, the status files, the panel's
  `options_ui.conf`, the online_tune cache — with properties beyond not
  crashing: every option within its declared range whatever the input
  (`Config::outOfBounds`), a profile never turning the layer on, a cache
  that reads back to itself. Every build replays the seed corpus and the
  inputs that once failed (ctest `fuzz_config`, `fuzz_status`,
  `fuzz_options_ui`, `fuzz_online_tune`).
- **Swapchain stress test** (`tests/swapchain_stress.sh`, ctest
  `swapchain_stress`): what games do to their swapchain, through the layer
  under the Khronos validation layer — the demo's new `--stress` modes
  (window resizes, swapchains recreated from the old one with another
  present mode and image count, surfaces and windows destroyed and made
  again, fullscreen toggles, minimization) every few frames, in
  interpolation, extrapolation, hybrid x3 with upscaling and HUD, x8 and
  adaptive; each run must exit cleanly with no VUID or SYNC message and
  generate at the multiplier again once the storm is over.
- **Flow confidence** (`flow_confidence`, opt-in pass at the flow's
  resolution): per flow texel, the backward vector and the forward one at its
  endpoint must cancel out on textured content (flat areas, which any vector
  warps the same, do not count — without that, the demo's flat squares read
  as untrusted) and pair the same luma. The share of uncertain flow goes to
  the statistics line, the status file, the panel and `--report` (demo: 0.7 %
  at a quarter speed, 1.9 % normal, 6.9 % at 3x); `debug=confidence` paints it
  red. `confidence_fallback` fades to the plain cross-fade there (binding 9 of
  the synthesis, full variant only): at 0.5 demo +0.20 dB, fast demo +0.02,
  synthetic corpus −0.2 to −1.1 dB, more flicker everywhere — left at 0.
- **Pacing graph** (`overlay = 6` / `pacing`, panel « + graphe de
  cadencement »): under the fps text, one column per present for the last
  64 — the game's frame time (green near the interval the pacing aims at,
  amber past 1.25x, red past 2x), grey without generated frames (catch-up,
  skip_fast, pause), a red mark when a frame was replaced before display.
  Push constants only (104 of the 128 bytes guaranteed), drawn by
  `hud_overlay.comp`; the panel also gained level 5 (latency).
- **Bug reports ready to send**: `bdex-framegen --report` ends with the link
  to a GitHub issue form (`.github/ISSUE_TEMPLATE/bug_report.yml`) filled
  from the report — version, distribution, GPU and driver, session,
  installation, the quick read; never the settings or the log, which stay
  in the attached report, and the home's path shown as `~`.
  `--issue-url FILE` makes the link for an existing report; the panel's
  « Rapport… » (and `b` in text mode) writes the report and opens the form.
  (The repository is private: the form opens for its collaborators only.)
- **Crash journal** (`crash_log`, Linux, on where the layer works): a
  SIGSEGV / SIGBUS / SIGILL / SIGFPE whose instruction (or one of its first
  four callers) is in the layer writes `~/.cache/bdex-framegen/crash-<game>-
  <pid>.txt` — signal, offsets in the layer for addr2line, version and
  settings — with async-signal-safe calls only, then goes on to the handler
  that was there (Wine's, Proton's) or the default action; every other
  fault is untouched. `--report` shows the latest three. Unit tests: the
  journal written then the default death, a previous handler still called.

### Input latency

- **`pipeline_cache`** (default on): the passes' pipelines through a
  `VkPipelineCache` loaded from and written back to
  `~/.cache/bdex-framegen/pipelines-<vendor>-<device>-<UUID>.bin` (checked
  against the driver's cache UUID, written atomically). NVIDIA without its
  own shader cache: frame generation ready in 0.15 s instead of 0.88.
- **`thread_priority`** (default on): the presentation thread gets the
  real-time round-robin class where permitted, else nice −10 from rtkit
  (D-Bus `MakeThreadHighPriority`, libdbus loaded at run time: no new
  dependency), else stays as it is. Wake-up lateness with 12 busy processes
  39 → 25 µs; frames shown unchanged on the headless bench (its compositor
  starves too).
- **`precise_wake`** (default on): the presentation thread drops the
  kernel's timer slack; wake-up lateness, now in the statistics line, 56 → 7
  µs idle, 130 → 38 µs with 12 busy processes (117.3 → 118.0 of 120 frames
  shown). Spinning the last half millisecond (sleep, then yield) was
  measured and rejected: 1 ms late and 114.7 frames shown under load.
- **`vrr_hold`** (default on): on a variable-refresh screen — the present
  feedback works but finds no fixed refresh grid (FreeSync / G-Sync, VRR
  compositors) — `interp_hold = 1` counts as auto: each frame shows when it
  is presented, so the real frame goes out as soon as the top rate allows
  (simulated, 40 fps x2 with a 144 Hz top: the real frame 3.8 ms sooner,
  every frame shown). Fixed-refresh screens unchanged.
- **Pacing simulator**: the pacing's decisions — present targets, the
  interp_hold bound, late jobs, the latency limiter, the adaptive multiplier,
  skip_fast's gate — moved, unchanged, into `layer/src/pacing.h` (no Vulkan),
  which the swapchain calls; `tests/test_pacing.cpp` drives them with a
  simulated game, GPU and mailbox screen (every frame displayed at x2 60 fps
  on 144 Hz; interp_hold 0.25 replacing frames without its bound, none with
  it; auto 2 ms sooner at 30 fps; adaptive 50 fps -> 144 frames a second;
  the limiter in closed loop; skip_fast's half second; jittered games).
  Each property checked to fail when its rule is broken.
- **Fixed (skip_fast, gamescope)**: after Steam's frame limit was lifted,
  gamescope's refresh cycle could stay the limit's, and a 30 fps game looked
  "at the screen's rate": its generated frames stayed off
  (`tests/gamescope_limit.sh`). skip_fast now ignores that cycle once a limit
  has been on, and otherwise takes the larger of the measured and the panel's
  rates.
- **Measuring input-to-screen latency**: `bdex_demo --latency-test [auto]`
  answers each key press (or an automatic input every half second) with a
  white frame and logs input and present times (`--latency-log`); the layer's
  `latency_log` logs every displayed frame (present feedback);
  `tools/latency_report.py` joins them — input to first photon and to the
  real frame, median / p95 / mean — and `tools/latency_bench.sh` runs
  settings side by side in a headless gamescope. First measurement (40 fps,
  144 Hz, first photon / real frame): x1 6.8 / 6.8 ms, interpolation x2
  10.0 / 19.9, with `interp_hold = auto` 9.2 / 16.2, extrapolation x2
  6.7 / 6.7. ctest `latency_probe` (skipped without gamescope).
- **`skip_fast`** (default on, fixed multiplier): no generation while the
  game already runs at 85 % of the screen's refresh rate or more (back under
  75 %, half a second either way): it would add almost no frame to the
  screen, only GPU work and the hold. Headless gamescope 144 Hz, game at
  130 fps x2: frame-to-screen 16.0 → 11.8 ms, the GPU spared the synthesis;
  100 fps still generates. The statistics line and the panel say so;
  unknown refresh rate: no effect (the Valheim case: 130 fps on 144 Hz).
- **`interp_hold = auto`** (or -1): the smallest hold the screen shows
  without losing a frame — 1.25 refresh between frames (exactly one lost 1.2
  of 80 frames a second to the frame-time jitter) — and only where it saves
  at least 2 ms; unknown refresh rate: even spacing. Headless gamescope
  144 Hz, x2: 30 fps 56.7 → 48.3 ms, 40 fps 43.8 → 40.7 ms, every frame
  displayed; 60 fps unchanged.
- **`interp_hold`** (interpolation, default 1 = unchanged bit for bit): the
  real frame's hold as a fraction of the usual one. Below 1 the generated
  frames go out closer together before the real one (`FrameOrder::offset`,
  the worker's `paceTarget`), each showing the content of its own display
  time (`t` = 1 − (real − p)·h/m), so motion stays right and only the
  spacing becomes uneven. Never closer than one refresh (`displayHz`: two
  frames in one refresh would replace each other — unclamped, 0.5 showed
  116 of 120 frames, 0.25 only 95). Measured in headless gamescope at
  144 Hz: 40 fps x2, frame-to-screen 43.4 → 37.6 ms, 79.3 of 80 frames
  displayed; 40 fps x3, 47.7 → 45.0 ms; 60 fps x2, 30.3 → 29.8 ms (the
  screen leaves little room). Panel: « Retenue de l'image réelle ».
- **The game's own Reflex / Anti-Lag**: when a game turns on
  `VK_NV_low_latency2`'s low-latency mode (and sleeps through it each frame)
  or `vkAntiLagUpdateAMD`, the layer's `latency_limiter` steps aside instead
  of stacking a second hold on the driver's; the statistics line says so.
  (Not tried with a Reflex game yet.)
- **`synth_priority`** (with `async_compute`): the synthesis queue at a high
  global priority (`VK_EXT/KHR_global_priority`), so a GPU-bound game's
  next frame does not delay the generated and real frames. Drivers grant it
  to privileged processes only: NVIDIA (and amdgpu without CAP_SYS_NICE)
  answer `VK_ERROR_NOT_PERMITTED`, and the device is created at normal
  priority (logged at INFO).
- **Fixed: device creation retries crashed under another layer.** The
  fallback attempts (without present feedback; now without the priority)
  called down with the layer link chain where the previous attempt had left
  it — a layer below (gamescope's WSI layer) had already advanced it, so the
  retry skipped it and the game crashed. Each attempt now starts again from
  the link after the layer's.

### Linux distributions

- **`bdex-capture` on Linux** (`capture/linux`): frame generation for a game
  window by capture, as the Windows capture app does — nothing loaded into
  the game: the games the layer stays out of (anti-cheat), non-Vulkan
  programs, and **two GPUs** (`--gpu`: the generation on the GPU driving the
  screen, the game on another one, Lossless Scaling's setup). X11 and
  Xwayland windows: XComposite + XDamage + MIT-SHM, the segments imported
  as host memory (no copy) where the driver allows it, an override-redirect
  overlay without input region following the game. Measured on an RTX 3060
  Ti, a 60 fps game: x2 at 10 ms from the frame's arrival to the screen
  (interpolation), 0.6 ms (extrapolation), hybrid x3 178 fps at 8 ms. Test
  `capture_linux` (Xvfb + lavapipe in CI): the ratio, and the overlay's
  pixels against the game window's. Not inside gamescope (games present
  through its WSI layer), nor native Wayland windows yet.
- **The control panel and the Windows launcher in English** when the
  system language is not French (`LANGUAGE`, `LC_ALL`, `LC_MESSAGES`,
  `LANG`; on Windows the display language), French otherwise;
  `BDEX_LANG=fr|en` chooses. The option texts come from
  `layer/options_ui.en.conf` (the build fails on an option without one),
  the panel's own through `tr()`, the status lines of the launcher too
  (`statusDescribe(…, english)`, which also names the hybrid mode now).
  Checked in both languages by `panel-check` (GTK 4 with and without
  libadwaita, text mode).
- **The Windows layer run for the first time** (`tools/ci/wine-layer-check.sh`):
  built with MinGW-w64, registered as an implicit layer like
  `register-layer.ps1`, loaded by Khronos' Windows loader (1.4.357) into a
  Win32 Vulkan program under Wine (winevulkan, lavapipe): generation on,
  x2.00. Its Windows code paths ran before only on paper. Two fixes on the
  way: the presentation thread's priority on Windows (the MMCSS "Games"
  task, else `THREAD_PRIORITY_HIGHEST`: it did nothing), and the 1 ms timer
  while the worker runs (`precise_wake`: paced waits otherwise last up to
  the default 15.6 ms on Windows).
- **`bdex-capture` under a window manager and a compositor** (openbox +
  picom, CI): x2 / x3 / x2 and the overlay showing the game, as without.
- **Proton on Wine's Wayland driver in CI** (`tools/ci/proton-wayland-check.sh`):
  the Direct3D 11 program under Wine 11 (WineHQ) on its Wayland driver — no
  X server, a headless weston — with DXVK: the layer's native Wayland path,
  x2.00. (The Wayland driver of the distribution's Wine 9 has no Vulkan.)
- **The Proton path in CI** (`tools/ci/proton-check.sh`, CI job
  `source-packages`): a Direct3D 11 program cross-compiled with MinGW
  (`tests/proton/d3d11_probe.cpp`) runs under Wine 9 with DXVK 2.5.3, and
  a Direct3D 12 one (`d3d12_probe.cpp`) with VKD3D-Proton 3.0.1 and DXVK's
  dxgi, on lavapipe; their swapchains reach the layer through winevulkan as
  a Proton game's do — loaded, generating, x2.00 in both. The display is Xorg's
  dummy driver with a 60 Hz mode: under Xvfb Wine reports 0 Hz and DXVK
  divides by it in its first Present.
- **The 32-bit layer at run time** (`tools/ci/lib32-check.sh`, CI job
  `source-packages`): built with -m32 in Debian 12 with the i386
  architecture, a 32-bit vkcube runs through it on Xvfb with the 32-bit
  lavapipe — loaded, generating, x2.00. (It had only ever been compiled.)
- **Flatpak Vulkan layer extension**
  (`packaging/flatpak/org.freedesktop.Platform.VulkanLayer.BdexFramegen.yml`,
  Freedesktop runtime 25.08, 64 and 32-bit, Flathub-ready): every Flatpak
  app on that runtime loads the layer, `flatpak update` updates it.
  `flatpak.sh` copies no layer into an app the extension covers (only the
  settings, and a mark the panel and `--report` read). Checked by
  `tools/ci/flatpak-check.sh`: built in a container, loaded by the runtime's
  64-bit loader, listed by its i386 one.
- **Source packages for build services**, each built from the checkout in
  the distribution's container, installed and checked (CI job
  `source-packages`): `packaging/rpm/bdex-framegen.spec` (COPR, OBS; Fedora 44
  and Tumbleweed, 32-bit layer included, no 32-bit glibc dependency),
  `packaging/debian/` (PPA, OBS; Ubuntu 24.04, 22.04, Debian 12, i386 layer
  in `lib/i386-linux-gnu`), a Gentoo overlay (`packaging/gentoo`, multilib
  `ABI_X86="64 32"`, `USE=gui`) and a Void template (glibc and musl).
- **`bdex-framegen --gpu screen | discrete | integrated | nvidia | amd |
  intel | list`**: the game on one GPU of a hybrid laptop (Mesa device
  selection, NVIDIA PRIME offload, DXVK / VKD3D-Proton device filter) — the
  GPU the screen is plugged into avoids a PRIME copy of every frame; the
  report's PRIME hint names it.
- `tools/uninstall.sh --purge`: also the settings and caches (and their
  copies in Flatpak / Snap launchers).
- `bdex-framegen --help` printed a fixed range of lines and had lost its
  last options.
- `docs/linux-validation.md`: what to try on real machines (Flatpak Steam,
  Deck, Snap, SELinux, NVIDIA + KDE, hybrid laptops...) and what to send.
- **Outdated sandbox copies**: after an update of the layer (package,
  archive, AUR...), the copies in the Flatpak / Snap launchers kept the old
  one. `flatpak.sh --check <prefix>` lists them; the control panel re-copies
  them when it opens (toast « Couche mise à jour dans… »), `bdex-framegen
  --check` exits 2 and `--report` flags them.
- **No copy over a running game's library**: `install.sh` (archive and
  source) and `flatpak.sh` replaced the installed `.so` in place, truncating
  the file a running game had mapped (SIGBUS). They now write a new file.
- **32-bit layer from source on Debian, Ubuntu, Fedora**: `tools/install.sh`
  looked for `/usr/lib32/libvulkan.so.1` (Arch's path only) and skipped the
  32-bit layer elsewhere. It now checks that a 32-bit C++ shared library
  links (verified in Ubuntu 24.04 and Fedora containers) and names the
  packages to install; `BDEX_NO_32BIT=1` skips it.
- **Refresh rate under Wayland (text-mode panel)**: read from `hyprctl`,
  `kscreen-doctor` (Plasma only), `wlr-randr` or GNOME's DisplayConfig before
  xrandr, which only sees Xwayland (143.78 Hz read for a 143.91 Hz screen
  under Hyprland). `bdex-framegen --report` reads hyprctl / wlr-randr too,
  and no longer hangs outside Plasma (`kscreen-doctor` waits forever
  without KScreen: now only in a Plasma session, under a timeout).
- **Terminal for the text-mode panel**: `$TERMINAL`, `xdg-terminal-exec`,
  Ptyxis, GNOME Console, foot, kitty, alacritty, wezterm... (Fedora 41+ and
  GNOME 47+ ship none of the old list).
- `--report` names the security modules (SELinux, AppArmor).
- Test `linux_scripts` (ctest, also in the `distros` job): sandbox copies,
  stale detection, re-copy while the library is open, removal, install.sh.

- **Distribution packages**: every release also ships `.deb` (amd64, arm64)
  and `.rpm` (x86_64, aarch64) packages of the prebuilt layer
  (`tools/package-native.sh`, from the release archives): installed to
  `/usr`, the layer in the distribution's library directory
  (`lib/x86_64-linux-gnu`, `lib64`; the 32-bit one in `lib/i386-linux-gnu`
  / `lib`). The layer finds its shipped profiles one level deeper for such a
  directory; `BDEX_INSTALL_LIBDIR` sets it for a build. Checked in CI:
  installed with apt on Ubuntu 20.04 to 24.04 and Debian 12, with dnf and
  zypper on Fedora and openSUSE Tumbleweed — loaded, profiles read, x2
  through vkcube.
- **Arch (AUR-ready)**: `packaging/arch/bdex-framegen-bin` (the release
  archive) and `bdex-framegen-git` (from source, now with the 32-bit layer,
  and the C++ runtime linked in like the archives), each with its
  `.SRCINFO`. Both built with makepkg and loaded in an Arch container.
- **NixOS**: `flake.nix` — the package, the 32-bit layer (`layer32`), and a
  module (`programs.bdex-framegen.enable`) that links `share/vulkan` into the
  system profile; CI job `nix` builds them and loads the layer.
- **Panel without GTK 4** (Ubuntu 20.04, Debian 11): text mode in a
  terminal — detected games, « Démarrer » / « Arrêter » (the same session
  and heartbeat), multiplier, mode, preset, fps counter, Game Mode, the
  Steam string; from the application menu it opens a terminal;
  `bdex-framegen-gui --text` forces it. Checked in CI (job `panel`: Ubuntu
  20.04 text only; Debian 12 and Arch, GTK 4 with and without libadwaita,
  and text).
- **Steam as a Snap**: the layer, its settings and the panel's session in
  `~/snap/steam` (written from snapd's layout; not tried on a real snap).
- `bdex-framegen --sandboxes [--remove]`: the layer copied into the Flatpak
  / Snap launchers — after a distribution package, which cannot write into
  the home.
- `bdex-framegen --mangohud`: MangoHud *under* the layer. On its own it
  loads above it (measured: it counted 330 fps for 660 shown, and drew on
  the real frames only).

### SteamOS / Steam Deck (gamescope)

- **Settings applied to the running game without the panel** (require_app
  = 0, Game Mode): a multiplier, mode or flow setting changed in the file
  rebuilds the output stage as under the panel (it only took the "live"
  options before — the Steam Deck menu's multiplier and mode waited for the
  next start); `require_app` switched in the file pauses or resumes the
  game. Test `live_config` (x2 → x3 → extrapolation → paused → resumed).
- **Decky menu: settings for the running game only** — its own `[section]`
  (from the status file's process name), started from the effective
  values; off returns it to the global settings.
- **Decky Loader plugin** (`packaging/decky`, `tools/ci/decky-check.sh`,
  zip attached to releases): the Steam Deck's quick access menu in Game
  Mode — frame generation in every game, multiplier (Auto, Adaptive, ×2–×4),
  mode, pause, FPS counter, and the running game's frame rates. The backend
  edits the configuration as the panel does (games' sections kept, one line
  per key) and the layer applies it to the running game within a second.
  Built and type-checked against `@decky/ui`/`@decky/api` in a Node
  container; the backend is tested without Decky (ctest `decky_backend`).
  Not yet tried on a Deck.
- **Game Mode without the control panel.** Game Mode runs one application
  at a time, so the panel cannot stay open beside a game: its new « Mode
  Jeu — sans le panneau » makes every game, or the selected one, generate
  from its start with the panel's settings (`require_app = 0` in `[*]` or in
  the game's section; `bdex-framegen --always-on [GAME]` / `--always-off`).
  Turned on for every game this way, desktop programs stay excluded, as
  under the panel — gamescope above all, Game Mode's compositor, which loads
  the layer too (before, `[*] enabled = 1` alone enabled it everywhere).
- **Steam's framerate limit.** The Quick Access Menu's limit caps what
  gamescope shows, generated frames included: a 30 fps game at x2 under a
  30 fps limit fell to 15 fps, 233 ms from present to screen, and after the
  limit a queue kept 100 ms more for good. The layer now reads gamescope's
  refresh cycle (its WSI layer answers `vkGetRefreshCycleDurationGOOGLE`:
  the display's, or the limit's) and the limiter state
  (`GAMESCOPE_LIMITER_FILE`). Under the limit it generates only up to it
  (adaptive, on the game's own frame time when it waits for the display),
  hands gamescope a frame once the previous one is on screen, and a game at
  the limit waits for its previous frame to be shown, like DXVK there.
  Measured in a headless gamescope 3.16 (SteamOS 3.9's): 30 fps kept,
  19 ms (as DXVK / VKD3D-Proton, "frame limiter aware") to 66 ms (native
  Vulkan) from present to screen; a 20 fps game under 30 is brought to 30,
  a 40 fps one under 60 to 60; x2 again after the limit, no queue left.
  gamescope never sends a swapchain it re-created (as its WSI layer forces
  when the limit changes) the current cycle: the layer keeps each surface's
  last one. The status (`--report`, the panel) shows the limit.
- **gamescope's refresh rate** feeds *Auto* / *Adaptatif* at once. The
  measured grid could not tell a 30 Hz display from a 60 Hz one presented to
  every other refresh: after a limit, x1 stayed at 30 fps on 60 Hz.
- **Presentation catch-up** (every system): a present whose game waited a
  quarter of a frame for the worker, or after eight jobs in a row one refresh
  or more late, shows its real frame alone — at the display's rate a queue
  never drained by itself. Default frames unchanged (the CI's bit-for-bit
  check and PSNR identical).
- **The game's swapchain handle**: calls the layer passes through — HDR
  metadata (the OLED Deck, HDR desktops), `VK_GOOGLE_display_timing`,
  `VK_NV_low_latency2` (Reflex through dxvk-nvapi), `vkGetSwapchainCounterEXT`,
  `vkSetLocalDimmingAMD` — reached a destroyed swapchain once the layer had
  re-created its real one underneath (a window resized by Wine after a mode
  change); they go to the current one. `vkWaitForPresent2KHR` returns at
  once, as `vkWaitForPresentKHR` already did.
- The control panel checked on SteamOS's versions — GTK 4.22, PyGObject
  3.56, no libadwaita — and on Debian 12 and Ubuntu 22.04
  (`tools/ci/panel-check.sh`, on a virtual display in a container). New
  tests: `gamescope_limit` (Steam's limit in a headless gamescope, skipped
  without one), `panel_logic` (the panel's config model).

### Compatibility

- **The prebuilt archives load on far more systems.** Built on Ubuntu 20.04
  with GCC 10 and the C++ runtime inside, the layer needs glibc 2.29
  (x86_64) instead of 2.36: Ubuntu 20.04 and 22.04 (Mint 20–21, Pop!_OS
  22.04, Zorin), Debian 11, openSUSE Leap 15.3+ and RHEL 9 now load it. The
  release build fails if a newer glibc symbol slips in, and the layer links
  with `--no-undefined` (it relied on the Vulkan loader for `dladdr` and the
  threads on glibc < 2.34). `tools/ci/package-release.sh` builds the
  archives in Docker, as the CI does.
- **An arm64 archive** (glibc 2.30): Asahi Linux on Apple Silicon, Snapdragon
  laptops, Raspberry Pi 5 — cross-compiled, installed the same way;
  `install.sh` refuses an archive of the wrong architecture.
- **The archives are checked where they will run**: every CI run installs
  them with `install.sh` in Ubuntu 20.04, 22.04 and 24.04, Debian 11 and 12
  and Fedora containers, and in Debian 11 and 12 arm64 trees under qemu-user
  (no binfmt needed), on each one's own glibc, Vulkan loader (1.2.131 to 1.4)
  and Mesa: the layer must load, and generate x2 through vkcube on a virtual
  display wherever that Mesa can present to one (all but Debian 11). A
  failure keeps them out of the release (`tools/ci/archive-check.sh`).
- **Manifests every loader reads cleanly**: `library_arch` lets loaders
  1.3.239 and newer skip the layer of the other width (the 32-bit one in a
  64-bit game) instead of logging a failed load; format 1.1.2, since loaders
  before 1.3 warned that the previous 1.2.0 was an invalid manifest version.
- **Steam, Heroic, Lutris or Bottles installed as a Flatpak.** Their sandbox
  sees neither `~/.local` nor `~/.config`: `install.sh` (and
  `tools/install.sh`) also install the layer inside each one found
  (`~/.var/app/<app>/data`, where its Vulkan loader looks), the control
  panel copies the settings there on every save and finds their games — in
  a process namespace of their own, it writes the session with the pid the
  game knows itself by and reads its status through the game's view of the
  files (`/proc/<pid>/root`); `bdex-framegen --report` and `--toggle`
  follow. `uninstall.sh` removes the copies.
- **Small GPUs.** The engine now fits in the limits every Vulkan GPU
  guarantees: `flow_refine` reads its lumas as textures (3 storage images per
  shader instead of 5, over the 4 guaranteed — the Raspberry Pi's V3D has
  4), and the block matcher's full variant (~21 KB of shared memory, 16 KB
  guaranteed) is only built where there are 32 KB, its optional terms off
  otherwise. A GPU below the guarantees gets its frames passed through with
  the reason, instead of a failed pipeline. Frames unchanged bit for bit;
  new test `min_limits` (`bdex_offline --limit-shared`,
  `--limit-storage-images` simulate a small GPU on any GPU).
- **Building from source on more distributions**: GCC 10 and later (the
  floating-point `from_chars` was GCC 11's), the Vulkan headers of Ubuntu
  22.04 and Debian 12 (1.3.204 / 1.3.239: `vk_compat.h` completes them),
  `glslangValidator` when `glslc` is missing (same SPIR-V for glslc builds),
  no Vulkan loader library needed for the layer; fixes for GCC 11, GCC 12
  (a `-Wrestrict` false positive with `-Werror`) and Clang. The CI builds
  in Ubuntu 22.04, Debian 12, Fedora, Arch, openSUSE Tumbleweed and Alpine
  (musl) containers and runs the demo through the layer there, on each
  one's own Vulkan loader and Mesa (`tools/ci/distro-check.sh`; on musl,
  Mesa's software driver compiles shaders with LLVM in threads whose 128 KB
  default stack it overflows — the check gives them glibc's 8 MB; GPU
  drivers do not compile with LLVM).

### Changed

- **Scene cuts need evidence beyond the flow cost** (`scene_cut_hist`,
  default 0.05): the fallback to the real frame (a fade of the whole picture,
  then a duplicate) also requires the brightness distribution to move — the
  earth mover's distance between the two frames' luma histograms, computed on
  a coarse pyramid level. Noise, film grain, foliage and fast motion raise
  the flow cost like a cut does, not the histogram: measured, real cuts 0.18
  and up, grain and fast motion under 0.02. Heavy grain: +0.11 dB, flicker
  −0.5 to −0.7, cuts still caught; normal content unchanged bit for bit (the
  gate lives in the cost reduction: the synthesis shader is untouched).
- **The zero-motion bias is capped** (`zero_bias_cap`, default 0.006, i.e.
  3 px at the default 0.002 per pixel): proportional to the displacement, it
  charged a true 16 px motion eight times a 2 px jitter, which broke fast
  pans (the free zero-motion candidate won, the picture cross-faded) while a
  static HUD only needs moving to cost something. Offline vs uncapped:
  smooth pan +2.7 dB, swing +2.4, occlusions +0.7, very fast demo +0.1, the
  demo and the HUD scene within ±0.06 dB. `0` restores the previous matcher
  bit for bit (the CI invariant pins both to 0).

- **Per-pixel vectors at motion boundaries by default on discrete GPUs**
  (`pixel_vectors`, with the automatic preset, up to 4K x2 / 1440p x3 /
  1080p x4 of synthesis work): offline, demo +0.3 dB, very fast demo +0.6,
  HUD scene +0.7, occlusions +2.9 dB. They are now tried only where the four
  surrounding block vectors and the looked-up one disagree by half a pixel or
  more — the same frames, for about half the added synthesis time.

- **Scene cuts per region by default on discrete GPUs** (`scene_cut_local`,
  with the automatic preset, where `pixel_vectors` is): a menu or overlay
  opening over part of the picture falls back to the real frame there
  instead of being warped — +1.4 dB, flicker −6.4 on a pause menu over a
  panning scene. Each tile now needs its own histogram evidence
  (`scene_cut_hist`), which removed its loss on very fast motion (−0.18 dB
  before, 0.00 now); elsewhere within ±0.01 dB.

- **Predicted frames splat forward and fill revealed areas by default on
  discrete GPUs** (`extrapolate_splat`, `reveal_fill`, with the automatic
  preset; extrapolation and hybrid modes): measured x3, extrapolation demo
  +0.9 dB, fast +1.2, very fast +0.35, pan +1.2, HUD +0.7; hybrid +0.2 to
  +0.6; occlusions −0.02, swing +0.02.

### Fixed

- **The worker acquired from a real swapchain already out of date.** A
  present that met `VK_ERROR_OUT_OF_DATE_KHR` (a fullscreen toggle) failed
  its job, but the job queued behind it still acquired from the same real
  swapchain — past the images the surface lets us hold, as the validation
  layer counts the images of a failed present as still acquired
  (`VUID-vkAcquireNextImageKHR-surface-07783`, CI's swapchain stress test,
  fullscreen toggles). The worker no longer acquires from a real swapchain
  that reported out of date: the jobs behind fail at once and the game
  re-creates its swapchain.
- **`debug = confidence` read past a table** in the configuration summary
  logged at start-up (four names for five values). Found by the fuzzer.
- **`FIFO_LATEST_READY` reached the real swapchain.** A game asking for it
  (a FIFO mode recent drivers report, VK_KHR_present_mode_fifo_latest_ready)
  slipped past "never present the real swapchain in FIFO", which only knew
  FIFO and FIFO_RELAXED — and the worker then queued its frames behind the
  vblank. It is now replaced by mailbox (or immediate) like them, and named
  in the log (it read "auto"). Found by the swapchain stress test.
- **Generated frames were lost whenever the GPU was busy.** The frames of a
  present were timed from the game's present call, but the game's rendering
  and the synthesis end on the GPU later — a frame later for a GPU-bound
  game. The generated frame and the real one then became ready together and
  the display showed only the second: x2 announced, x1 shown (measured, a
  9 ms synthesis at 66 fps: every generated frame replaced before display).
  The pacing now starts when the GPU has finished the synthesis, every copy
  is submitted at once (the game is never held back by the pacing), and only
  the presents are timed. Same measurement: all 132 frames displayed.
- The copies and presents run on a queue of their own, the synthesis on
  another of the same family (`present_queue`, default on where the family
  has two queues to spare): a present's GPU work (the presentation blit of
  X11 / Xwayland) waited behind the next frame's synthesis — the real frame
  shown 7 ms late, half the generated frames replaced.
- Extrapolation: a real frame arriving early now drops the predicted frames
  still waiting (they are obsolete). The present used to wait for them first,
  holding the new real frame back by up to (m − 1)/m of a frame.
- **Hybrid and extrapolation showed a GPU-bound game at x1.** The predicted
  frames still waiting were dropped as soon as the game *called* its next
  present — but a GPU-bound game calls it a frame before its rendering ends,
  and the predicted frames are ready before that. They are now dropped only
  once the next present's first frame is ready on the GPU. Hybrid's (and a
  packed extrapolation's) predicted frames are copied into staging images at
  once, so the game's next present no longer waits for the real frame's
  pacing either. Measured, 4K x3 hybrid, GPU-bound: 116 → 283 frames
  displayed a second (all), game 58 → 94 fps, frame-to-screen 34.5 → 25.5 ms
  (18.3 ms with `latency_limiter`); interpolation unchanged.
- The refresh rate learned from the displayed frames could be a fraction of
  the real one: with every other frame replaced, the displayed intervals all
  span two refreshes (seen: « 115 Hz » on a display that showed 200 fps).
  The rate the control panel reads is now a hint the measurement must
  contradict to be ignored; « Auto » and `fps_limit = auto` follow it.
- `online_tune` ran part of its bookkeeping on the worker thread (the copy
  slots called the passes' slot hook), racing with the application thread
  and, with `async_compute`, reading its sums before the GPU wrote them.
  The GPU profiling counters shared by both threads are locked.

### Added

- **Up to x8** (`multiplier` 2 to 8; the control panel's « Auto » and
  « Adaptatif » raise their ceiling with the screen: x4 up to 165 Hz, x6 up
  to 250 Hz, x8 above). Measured through the layer: 30 fps into 240 fps on a
  240 Hz output, 239.3 fps displayed. The frames of a present the real
  swapchain cannot give an image to at once (a surface allowing few images:
  6 of 8 frames under gamescope) are copied into staging images right away,
  then to their swapchain image when presented — before, the game waited
  for them (6.8 fps instead of 30).
- **Adaptive multiplier** (`adaptive = 1`, « Adaptatif » in the control
  panel and the capture launcher): every game frame gets as many frames as
  fit at the target rate — `target_fps`, or the display's refresh rate — the
  fraction carried over to the next one, each synthesised at its own display
  time and presented one output period apart. 50 fps on 144 Hz: 3, 3, 2, 3…
  frames, **144 fps displayed** instead of the 100 of the largest whole
  multiple (measured through the layer); works with every mode (hybrid:
  139.7 fps displayed, 12.4 ms present-to-screen). `multiplier` is the
  ceiling. The capture app reads the refresh rate of the overlay's monitor.
- **`mode = hybrid`** (x3/x4): one interpolated frame, then the real frame,
  then predicted ones. The real frame is shown 1/m of a frame late instead
  of (m − 1)/m — at x4, a quarter of a frame instead of three quarters —
  and the predictions reach at most (m − 2)/m ahead instead of (m − 1)/m.
  Offline on the demo (normal and fast): x4 −1.3 to −2.4 dB from
  interpolation, against −3.0 to −4.7 dB for extrapolation; x3 −1.0 to
  −1.4 dB, against −3.4 to −4.4 dB. In the control panel, the capture
  launcher and `bdex_capture --mode hybrid`. The frame order is one
  definition shared by the layer, the capture app and `bdex_offline`
  (`frame_order.h`); interpolation and extrapolation are unchanged bit for
  bit.
- **`latency_limiter`** (default on, with `max_frame_latency`): when the GPU
  is the bottleneck, the game's present returns just in time for its next
  frame to reach the GPU as it frees up — the game reads its input later
  for the same frame on screen, like Reflex or Anti-Lag, in every Vulkan
  game. Steered by how long the game waits for the GPU, with a slack that
  follows its frame-time jitter; a game that is not GPU-bound is never
  held. Measured, GPU-bound demo at 4K x2: frame-to-screen 26.8 → 18.3 ms,
  game 97 → 101 fps; light and capped runs unchanged; with the CPU busy
  elsewhere, −6 % of the frame rate. The statistics line shows the hold
  (`next frame held N ms`).
- `max_frame_latency` (default 1 while generating): the game's present
  returns once the GPU has finished its previous frame, like a driver's
  low-latency mode. A GPU-bound game otherwise queues frames ahead, each a
  frame interval of input lag (measured, GPU-bound demo at 4K: frame-to-screen
  50 → 36 ms, and 60 → 71 fps from less GPU contention). `0` = no limit.
- Statistics: `displayed N fps` (frames really shown, where the present
  feedback tells) and `frame-to-screen` (from when the game began a frame —
  its previous present returned, where most games read their input — to the
  screen). The fps HUD shows the displayed rate instead of game fps ×
  multiplier when it is known.
- `bdex-framegen --report` compares the frames sent with the frames really
  displayed (last statistics line) and says what to try when a share of them
  never reaches the screen.
- `bdex_capture --adaptive` (the launcher's « Adaptatif » on the command
  line); `--multiplier`, if given, is its ceiling.

## [0.5.5] - 2026-09-26

### Fixed

- Numbers in the config file are read with a dot whatever the process's
  locale: a game or toolkit running with a French (decimal comma) locale
  rejected `scene_cut_low = 0.3` as an ignored option, and a process in the
  C locale rejected `0,3`. The status files the panel and the report read
  are written with a dot too.
- The control panel re-reads the config file before saving it: lines added
  by hand while it was open ([game] sections, other keys) were erased by its
  next save (« Démarrer », any setting).
- The scene-cut statistics of 0.5.4 read the mean cost while the GPU could
  still be writing it (a data race ThreadSanitizer reported in CI): it is
  now copied per command slot and read after that slot's fence.

## [0.5.4] - 2026-09-26

### Added

- The statistics line reports the **scene-cut fallback**: the share of
  generated frames replaced by a copy of the real frame (mean matching cost
  above `scene_cut_high`) or partly faded to it, with the mean cost. Such
  frames are displayed but show nothing new, so x2 looks like x1 while every
  counter says x2. `debug = flow` does not reveal it (its colours are drawn
  after the fallback).
- `--report` quick read: warns when that fallback exceeds 30 % (and suggests
  `scene_cut_low` / `scene_cut_high` for the game), and when the layer sends
  more frames than the screen's refresh rate (the compositor drops the
  excess: the game already runs near the refresh rate). The report lists
  the Vulkan layer order (Steam overlay above or below the layer).

## [0.5.3] - 2026-09-26

### Added

- `bdex-framegen --report` starts with a **quick read** (in French, like the
  panel) of the usual causes of « no generated frames »: « Auto » at x1
  because the layer's refresh rate is at or under the game's fps, a refresh
  rate that differs from the desktop's (screen misdetected) or is unknown,
  the panel closed or targeting another process, the game on a GPU no
  screen is plugged into (hybrid laptops / iGPU + dGPU desktops). The report
  also lists the screens (kscreen-doctor, xrandr), which GPU each connected
  output is on, the GPUs (lspci) and the panel's session (target pid and the
  refresh rate it read).

## [0.5.2] - 2026-09-26

### Added

- **« Auto » multiplier in the control panel** (the default for a new
  config; `multiplier = 4` + `target_fps = auto`): for each game frame, as
  many frames as fit **under the screen's refresh rate** — on 144 Hz, 60 fps
  gives x2 (120), 45 fps x3 (135), 36 fps x4 (144), and no generation from
  74 fps — without slowing the game down (mailbox, unlike FIFO). Above the
  refresh rate a mailbox display drops frames unevenly, which looked like no
  gain at all (SteamOS, KDE, NVIDIA: 60 fps x4 = 240 on 144 Hz).
- The panel reads the refresh rate from the system (GDK) and passes it in its
  session; the layer uses it where it cannot measure the display itself (no
  present feedback, no refresh grid: KDE + NVIDIA). Neither known: x2.

### Changed

- `target_fps`: the largest multiplier at or under the target (was the
  nearest: 50 fps on 144 gave x3 = 150), down at once past it, up with a 3 %
  margin (`dynamicMultiplier`, unit-tested).

### Fixed

- `target_fps = -1` / `fps_limit = -1`, which the panel's advanced spin
  buttons write for "auto", were clamped to 0 (off) without a warning.
- `tests/integration.sh` ignores the user's config (it read multiplier = 4).

## [0.5.1] - 2026-09-26

### Fixed

- **Control panel without libadwaita** (SteamOS, Debian 12, Ubuntu 22.04:
  none, or older than 1.4): it stopped at start-up with « Namespace Adw not
  available ». The widgets it uses are now also built from plain GTK 4
  (`_Compat`), used when libadwaita 1.4+ is missing; the panel needs only
  Python 3, PyGObject and GTK 4. `BDEX_FG_GUI_NO_ADW=1` forces that path.
- **Electron / Chromium apps (Discord) were armed as games**: their command
  line is rewritten as one string, so the name checks missed them. The layer
  also checks its executable's name (`platformExeName`), the panel the
  binary and the first word.
- Panel: no horizontal scrolling, long texts wrap to the window's width.
- The prebuilt archive's README gives the real requirements (glibc 2.36+,
  GTK 4; libadwaita optional) and says Flatpak / Snap Steam cannot see the
  layer.

## [0.5.0] - 2026-09-26

### Added

- **Linux control panel works like the Windows launcher** (`require_app`,
  which the panel writes with `enabled = 1`): launch the game normally,
  pick it in « Jeux détectés » (the running Vulkan programs, native or
  Proton, found in `/proc/<pid>/maps`, refreshed with « Actualiser la
  liste »), « Démarrer » — generation starts in the running game;
  « Arrêter » or closing the panel stops it. Every Vulkan game is armed
  (virtual swapchain, frames passed through) except desktop programs
  (browsers, Steam, GTK 4 apps, which draw with Vulkan); the session file
  `app.session` = "<panel pid> <target pid>" tells the layer which process
  generates. Options changed while a game runs apply live: settings that
  size the output stage (multiplier, mode, flow, HUD) rebuild it, like a
  resize. Upscaling applies to games started while the panel is open.
  `BDEX_FG=1` (Steam option, CLI) bypasses it. Covered by a new "panel" run
  of `tests/validation.sh`, and checked with the demo inside Steam's
  container (SteamLinuxRuntime_sniper).

### Changed

- **Linux status / session directory: `~/.cache/bdex-framegen/run`** (was
  `$XDG_RUNTIME_DIR/bdex-framegen`). Steam's container (pressure-vessel, every
  Proton game) shares the home but only a few sockets of the runtime
  directory: Proton games' status never reached the panel and the panel's
  session never reached them (PRAGMATA could not be started). The session is
  a heartbeat — the panel rewrites it every second, older than 5 s means
  closed — instead of a lookup of the panel's process.
- **The layer stays off in competitive multiplayer games**, matched by
  executable name, where the anti-cheat has no process to detect: VAC
  (CS2, CS:GO, Dota 2, Deadlock, TF2), Vanguard (Valorant, League of
  Legends), Ricochet (Call of Duty), Overwatch 2, Marvel Rivals, and EAC /
  BattlEye games whose anti-cheat runs inside the game under Proton (Apex,
  Fortnite, Rainbow Six Siege, PUBG, Rust, Dead by Daylight, Halo Infinite,
  Destiny 2, Tarkov, Rocket League, Squad, War Thunder, World of Tanks).
  Whatever the config says, as for the detected anti-cheats.

### Changed

- **Windows capture: interpolation by default**, as in the layer (was
  extrapolation). Extrapolation's predictions break down on the larger
  motion of low frame rates — visibly more artefacts than the Linux layer
  with the same defaults. `--mode extrapolate` or the launcher keep it
  available; a mode already saved by the launcher is kept.

### Fixed

- **Windows capture pacing**: the waits between the frames of a job used
  `Sleep` / `WaitForMultipleObjects` at the process's default 15.6 ms timer
  resolution (per process since Windows 10 2004), so a 16.7 ms wait (x2 at
  30 fps) lasted up to 31 ms: the generated frame reached the screen next to
  the real one, which came up to a period late; in extrapolation the
  predicted frame was often dropped. The app now sets a 1 ms resolution and
  sleeps until ~1.5 ms before each deadline, then spins on the clock.

## [0.4.0] - 2026-09-25

### Added (release)

- **Prebuilt Linux archive** `bdex-framegen-linux-x86_64-<version>.tar.gz`
  (`tools/package-linux.sh`, CI job `package`, attached to the GitHub
  release with the Windows zips): 64- and 32-bit layer, the CLI, the
  control panel, profiles, `install.sh` / `uninstall.sh` — nothing to
  compile. Built on Debian 12 (glibc 2.36+) with newer Khronos headers; the
  layer links libstdc++/libgcc statically (`BDEX_STATIC_RUNTIME`) so it
  loads inside the Steam runtime.

### Changed (release)

- **`zero_bias_contrast` defaults to `0.02`** (was `0`, the absolute zero
  bias) on Linux and Windows alike. Offline, on the synthetic corpus and the
  demo replay, interpolation and extrapolation: never below −0.01 dB; demo
  +0.10 / +0.83 dB, smooth pan +9.6 / +6.4, swing +2.3 / +0.9, flicker down;
  no measurable GPU time (RTX 3060 Ti, 1440p). `flow_temporal`, measured with
  it (+0.1 dB more), stays opt-in: ~0.28 ms of GPU time at 1440p with
  `preset=quality`. `zero_bias_contrast=0` gives the historical path, which
  the f2cec5f invariant check now sets explicitly.

### Fixed (2026-09-25)

- **GPU cost of the default path halved (latency in interpolation).** The
  opt-in terms added to the block matcher since 2026-09-14 (census,
  rotation, chroma, gradient) sized its shared memory for all of them
  (~21 KB a workgroup against ~3.4 KB), whatever their weights: half the
  blocks in flight per SM, flow ~2x slower. The synthesis carried the same
  weight in registers (Catmull-Rom taps inlined in every sample,
  pixel_vectors, occlusion...). Both shaders now have a specialization
  constant `kOptIns`; the layer builds a second, lean pipeline with it off
  and binds it while those options are 0 (`Pass::lean`). Measured on an
  RTX 3060 Ti, 1440p output, preset quality, render_scale 0.67: flow
  1.9-2.0 -> 0.8-0.9 ms, synthesis 0.75 -> 0.55 ms per real frame. The
  matcher's exported cost is also summed from per-thread loads instead of
  64 dependent image loads on one thread (same order, same value). Frames:
  byte-identical to `f2cec5f` on NVIDIA again (main had drifted by 1 LSB in
  interpolation through code generation) and on lavapipe; byte-identical
  to main with every option on.

### Added (2026-09-25)

- **`preset = enhanced`**: balanced flow plus the opt-ins measured positive
  *together* — `zero_bias_contrast=0.02`, `pixel_vectors`, `flow_temporal`,
  `hud_mask=24`, `scene_cut_local`, `dedup`, `extrapolate_splat`. Offline,
  x2 vs the defaults: interpolation +0.08 to +10.8 dB (demo +0.71, fast
  demo +0.28, occlusions +2.8, HUD +0.75), extrapolation −0.04 to +7.1 dB
  (demo +2.0); about twice the synthesis time. The bundle measurement also
  showed `zero_bias_contrast=0.1` losing 1.2 dB on the demo replay with
  `pixel_vectors` (1.7 dB alone, where the online eval had measured +0.02),
  hence 0.02. In the GTK panel and the Windows launcher preset lists.
- `determinism` test: every option (and all of them together, per mode)
  runs twice through bdex_offline on the `occlude` scene and must give
  byte-identical frames — a shader reading what it did not write or a race
  between invocations shows at once (as rot_weight's out-of-tile reads
  did). The option list comes from the registry: a new option fails the
  test until it is exercised or declared frame-neutral. 66 runs, ~30 s.
- **Windows: a notification-area icon while capturing.** Its tooltip shows
  the live status (captured game, fps, latency — the same French summary as
  the launcher and the GTK panel), its menu toggles frame generation, shows
  the detailed status (passes, `online_tune`) and quits — the hotkeys'
  actions, for players who do not remember them. The icon comes back when
  Explorer restarts.
- **Recorded game sequences in the CI.** `bdex-framegen --record DIR [N]`
  records a game's real frames; `tools/corpus_pack.py` packs a downscaled
  window of them (PNG + meta.json), `tools/corpus_fetch.py` unpacks
  archives into sequence directories. The Linux CI downloads the archives
  of the repository's `corpus` release, when it exists, and scores them in
  the corpus regression test (`BDEX_CORPUS_DIR`). CONTRIBUTING.md explains
  how to add a game. No game sequence ships yet: they have to be recorded
  on a machine with the games.
- `bdex-framegen --tuned-profile [GAME] [--mode M] [--apply]`: the options
  `online_tune` kept for a game (pairs expanded) as a `[GAME]` config
  section — printed (ready to share or to add to `layer/profiles.conf`), or
  merged into the config file, which pins them (the tuner leaves options
  set explicitly alone). Without GAME, lists the tuned games and modes.
- **`reveal_fill` (opt-in, extrapolation)**: in areas a moving object
  uncovers, the source search alternates between the object and the
  background; the pixel now takes the source whose content moves less (the
  background — the model `extrapolate_splat` already uses), where the
  trajectory is also inconsistent. Revealed areas +1.9 dB on the new
  `occlude` corpus scene, pan +1.04, HUD +0.58, swing +0.39; demo −0.33
  (−0.07 with `extrapolate_splat`). An `online_tune` candidate.
- Corpus: `occlude` (an object crossing a panning background) with masks
  of the areas it uncovers or covers; `eval_sequence.py` scores them in an
  `occl` column for any sequence that ships `occlusion/<frame>.png`. It
  showed that in interpolation `pixel_vectors` handles occlusions well
  (+1.3 dB there); a per-trajectory visibility test (a trajectory whose far
  end is hidden judged by its visible sample) lost 1 dB and was dropped.
- **`online_tune` tries pairs.** Options measured to pay only together are
  declared as pairs in the registry (`kTunePairs`) and tried as one
  candidate (key `a+b`, value `x+y`, expanded when applied and cached as
  is): in interpolation `subpixel_lk=2 + synth_bicubic=1` and
  `subpixel=1 + synth_bicubic=1` (alone, sub-pixel vectors lose 0.8 dB
  there). The probe rates them; on the corpus they are dropped as they
  should be — neutral on the swinging and HUD scenes, and on the sub-pixel
  pan their +0.37 dB sits at 52 dB, below the 1 % bar.
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

- `gpu_budget`'s first level also turns off `synth_bicubic` and
  `subpixel_lk` (the costly opt-ins added since). The README's
  `ZERO_BIAS_CONTRAST` row notes the offline demo replay's loss at 0.1.
- `pixel_vectors` with `synth_bicubic`: a candidate vector equal to the
  current one or to one already tried is skipped (same error, rejected by
  the strict comparison) — away from motion boundaries one evaluation
  instead of four: synthesis 6.1 → 2.1 ms on a 1.7 Mpx frame (RTX 3060 Ti),
  output byte-identical. Not done with bilinear samples, where the test
  costs more than it saves.
- **One description of the option controls for both UIs.**
  `layer/options_ui.conf` holds the sections, labels, help texts and UI
  ranges; kinds, registry ranges and defaults come from the option
  registry (`Config::optionInfo`). The GTK panel builds its sections from
  `options_ui.json` (generated at build time by `tools/options/bdex_options`,
  which fails on an unknown key, and installed next to the panel); the
  Windows launcher embeds the `.conf` and builds its option column from it,
  its window sized to the content. Both used to keep hand-written lists.
  The GTK panel's option list is unchanged (checked against the old one),
  except the updated `online_tune` help.
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

- GTK panel: option texts are escaped before display — a help text with
  "< 1" (`upscale_temporal`) was taken for Pango markup and not shown.
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

[Unreleased]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.6...HEAD
[0.5.6]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.5...v0.5.6
[0.5.5]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.4...v0.5.5
[0.5.4]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.3...v0.5.4
[0.5.3]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.2...v0.5.3
[0.5.2]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Bdexez/bdex_frame_gen/releases/tag/v0.1.0
