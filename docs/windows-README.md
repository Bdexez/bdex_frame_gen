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
   options (generation on/off, ×2/×3/×4, extrapolation/interpolation, quality
   preset, overlay size, upscale filter and sharpness, vsync, fps counter)
   and press **Démarrer**. The choices are saved to
   `%APPDATA%\bdex-framegen\bdex-framegen.conf` (the same keys as the Linux
   control panel); when the capture stops (Ctrl+Alt+Q, game closed) the
   launcher reopens.
3. An always-on-top overlay appears over (or, with "Ajustée à l'écran",
   centred on the monitor of) the game window, showing the game with
   generated frames in between: x2 by default, **extrapolation** by default
   (interpolation is smoother but adds half a frame of latency). The fps HUD
   prints `capture > output`; the console logs `capture → shown → output`
   every 2 s.

   Command line (same options, scriptable — giving any argument keeps the
   old behaviour of capturing without the window):
   ```
   bdex_capture.exe --list                            # which windows can be captured
   bdex_capture.exe --title "Kenshi" --fit --hud 2    # by title, fill the monitor, fps HUD
   bdex_capture.exe --pid 1234 --multiplier 3         # by process id, x3
   bdex_capture.exe --title "Kenshi" --mode interpolate
   bdex_capture.exe --hud 2                           # no --title/--pid: foreground, 5 s
   ```
4. **Ctrl+Alt+Q** quits, and so does closing the game.

The frame-generation engine is the Linux layer's (`framegen.cpp` and its
shaders, unchanged): the quality presets, `--filter`, `--sharpness`, and every
other option of `bdex-framegen.conf` (`%APPDATA%\bdex-framegen\`) or the
`BDEX_FG_*` environment apply. The GPU is the discrete one by default
(`--gpu n` to choose; the list is logged at start), and the D3D11 capture
device is created on the same adapter.

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
