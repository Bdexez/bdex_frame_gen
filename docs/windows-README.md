# bdex-framegen on Windows — quick start

This folder (the `bdex-framegen-windows-<arch>` zip built by the `windows`
GitHub Actions workflow) contains **two products**:

| File | What it is | Works with |
|---|---|---|
| `VkLayer_bdex_framegen.dll` + `register-layer.ps1` | **Mode 1** — the Vulkan layer (frame generation + upscaling) | **native-Vulkan** games and emulators only (Doom, Baldur's Gate 3 in Vulkan mode, RPCS3, Dolphin, yuzu…) |
| `bdex_capture.exe` (x64 zip only) | **Mode 2** — the capture overlay with frame generation | **any** windowed/borderless game (D3D9–12, OpenGL, Vulkan); no injection |

Both need a Vulkan-capable GPU driver (any NVIDIA / AMD / Intel driver from
the last few years). Mode 2 has been validated on real hardware (D3D9/11/12
games); mode 1 is CI-built but not yet validated on a real machine — see the
checklist in `windows-port.md` §3. Please report what happens.

## Mode 2 — `bdex_capture.exe` (any game)

1. Put the game in **windowed or borderless** mode (exclusive fullscreen cannot
   be overlaid — same limitation as Lossless Scaling).
2. Double-click `bdex_capture.exe` (or run it with no arguments): a
   **launcher window** opens — pick the game's window in the list, set the
   options (generation on/off, ×2 to ×8 or **Adaptatif** — as many frames per
   captured frame as the monitor's refresh rate allows, 50 fps on 144 Hz
   showing 144, not 100 —, interpolation/extrapolation/hybrid, quality
   preset, overlay size, upscale filter and sharpness, vsync, fps counter,
   and in the right-hand **Options avancées** column every other engine
   option: motion quality — contrast-relative stability (0.1 recommended),
   object edges, sub-pixel motion, splatted extrapolation — the experimental
   ones, the flow structure and the debug view) and press **Démarrer**. The choices are saved to
   `%APPDATA%\bdex-framegen\bdex-framegen.conf` (the same keys as the Linux
   control panel); when the capture stops (Ctrl+Alt+Q, game closed) the
   launcher reopens. Its **En cours** frame lists, refreshed every second,
   what is running with bdex-framegen: Vulkan games through the layer
   (mode 1) and captures — fps, mode, latency, the optional passes at work
   and `online_tune`'s decisions or current trial. While a capture runs,
   its **notification-area icon** shows the same status in its tooltip; its
   menu toggles generation, opens the detailed status and quits (as
   Ctrl+Alt+G / Ctrl+Alt+Q). The files are `%TEMP%\bdex-framegen\<pid>.status`.
3. An always-on-top overlay appears over (or, with "Ajustée à l'écran",
   centred on the monitor of) the game window, showing the game with
   generated frames in between: x2 by default, **interpolation** by default, as on Linux
   (extrapolation saves half a frame of latency but shows far more
   artefacts at low frame rates; **hybrid**, at x3 and above, shows one
   interpolated frame, the real one, then predictions — the real frame is only
   1/m of a frame late, with far fewer artefacts than pure extrapolation). The fps HUD
   prints `capture > output`; the console logs `capture → shown → output`
   every 2 s.

   Command line (same options, scriptable — giving any argument keeps the
   old behaviour of capturing without the window):
   ```
   bdex_capture.exe --list                            # which windows can be captured
   bdex_capture.exe --title "Kenshi" --fit --hud 2    # by title, fill the monitor, fps HUD
   bdex_capture.exe --pid 1234 --multiplier 3         # by process id, x3
   bdex_capture.exe --title "Kenshi" --mode interpolate
   bdex_capture.exe --title "Kenshi" --adaptive       # fill the monitor's refresh rate
   bdex_capture.exe --title "Kenshi" --mode hybrid --multiplier 4
   bdex_capture.exe --hud 2                           # no --title/--pid: foreground, 5 s
   ```
4. **Ctrl+Alt+Q** quits, and so does closing the game. **Ctrl+Alt+G**
   toggles frame generation on and off without stopping the capture (off = the
   real frames pass straight through). Rebind it with a `hotkey` line in the
   config file, e.g. `hotkey = ctrl+shift+f1` (modifiers `ctrl`/`alt`/`shift`/
   `win` + a letter, digit, or `F1`–`F24`).

**Per-game profiles.** In the launcher, tick **"Profil pour ce jeu"** before
Démarrer to save the options into a `[game]` section (named after the target's
executable) instead of the global ones; selecting a game that already has a
profile loads it and ticks the box automatically. On the command line the same
sections apply — `bdex_capture` reads the config as the *target* game, so a
hand-written `[DarkSoulsIII]` section overrides the globals for that title. A
per-game section overrides the global one only for the game it names.

The frame-generation engine is the Linux layer's (`framegen.cpp` and its
shaders, unchanged): the quality presets, `--filter`, `--sharpness`, and every
other option of `bdex-framegen.conf` (`%APPDATA%\bdex-framegen\`) or the
`BDEX_FG_*` environment apply. The GPU is the discrete one by default
(`--gpu n` to choose; the list is logged at start), and the D3D11 capture
device is created on the same adapter.
**HDR.** When the game's monitor is in HDR mode (Windows "Use HDR"), the
capture runs in scRGB end to end — FP16 capture, an scRGB overlay — so HDR
games keep their highlights; the log says `colour: HDR (scRGB, FP16)`. If the
GPU or driver cannot carry FP16 from D3D11 to Vulkan, or the overlay has no
scRGB format, it stays in SDR as before. `capture_hdr = 0` forces SDR, `1`
HDR where possible (launcher: "Couleur HDR"). Not yet run on HDR hardware.

For capture, `mode` defaults to interpolation (as in the layer), then the config file and
`BDEX_FG_MODE` take precedence. An explicit `--mode` wins over those settings
and over `--preset`; the mode selected in the launcher wins over all of them.

After registering the layer, run `powershell -File .\validate-windows.ps1` to
check registry and loader discovery plus capture window enumeration. For the
first on-hardware runs, add `-GameExe <native-Vulkan-game.exe>` and/or
`-WindowTitle <window-title>`; the script checks generation and records logs
under `%TEMP%`. For the Win32 layer, use `-Wow6432` and supply a 32-bit
`vulkaninfo.exe` with `-VulkanInfo` to check loader discovery.

The periodic console statistics include mean/p95 time from retrieval of a WGC
frame from its pool to the Vulkan synthesis submit and to the real frame's
`vkQueuePresentKHR` call. These timings exclude the game's own present-to-WGC
delay and display scanout.

Known limits: capture itself costs about one frame of latency (see
`windows-capture.md` §0); keyboard input reaches the game (the overlay never
takes focus) but mouse clicks pass through only where Windows honours
`WS_EX_TRANSPARENT` hit-testing — if the game stops reacting to the mouse,
run without `--fit` or use `--scale` < 1 so part of the game window stays
uncovered. Set `BDEX_CAP_VALIDATION=1` to load the Khronos validation layer
when reporting a problem.

## Mode 1 — the Vulkan layer (native-Vulkan games)

```powershell
powershell -ExecutionPolicy Bypass -File .\register-layer.ps1 -Dll .\VkLayer_bdex_framegen.dll
# 32-bit games: use the Win32 zip and add -Wow6432
```

Then create `%APPDATA%\bdex-framegen\bdex-framegen.conf`:

```ini
enabled=1
log_file=%TEMP%\bdex-fg.log
multiplier=2
hud=2
```

`vulkaninfo --summary` should list `VK_LAYER_BDEX_framegen`. Launch a Vulkan
game: the HUD shows `game > output`. `unregister-layer.ps1` removes the layer;
`BDEX_FG_DISABLE=1` in the environment skips it for one launch. Details and
the full option list: `windows-port.md` and the main README.

**Known limitation (mode 1, not yet validated on hardware):** the layer
virtualises the swapchain and does **not** yet forward
`VK_EXT_full_screen_exclusive`. A native-Vulkan game asking for true
exclusive fullscreen will fall back to a composited (borderless-like) present
through the layer. For the first tests prefer **borderless / windowed**
fullscreen, which is the well-trodden path. Forwarding FSE is a planned
phase-2 item. If a game shows nothing or stutters in exclusive fullscreen,
switch it to borderless.

### Per game, from Steam launch options

To turn the layer on for a single game there is nothing to write in the
config file: put this in the game's Steam launch options. Steam executes
them through cmd on Windows, so the `BDEX_FG=1 %command%` syntax used on
Linux is not valid there — `set` defines the environment the game process
inherits, which is what the layer reads:

```
cmd /c "set BDEX_FG=1 && %command%"
cmd /c "set BDEX_FG=1 && set BDEX_FG_MULTIPLIER=3 && set BDEX_FG_MODE=extrapolate && %command%"
```

Any `BDEX_FG_*` option works this way and overrides the config file; add
`log_file=%TEMP%\bdex-fg.log` to the config file once, since game stderr
goes nowhere on Windows. `cmd /c "set BDEX_FG=0 && %command%"` forces the
layer off for one game. Per-game `[name]` sections in the config file are
the third way and need no launch options at all.
