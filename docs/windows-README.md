# bdex-framegen on Windows — quick start

This folder (the `bdex-framegen-windows-<arch>` zip built by the `windows`
GitHub Actions workflow) contains **two products**:

| File | What it is | Works with |
|---|---|---|
| `VkLayer_bdex_framegen.dll` + `register-layer.ps1` | **Mode 1** — the Vulkan layer (frame generation + upscaling) | **native-Vulkan** games and emulators only (Doom, Baldur's Gate 3 in Vulkan mode, RPCS3, Dolphin, yuzu…) |
| `bdex_capture.exe` (x64 zip only) | **Mode 2** — the capture overlay, phase-1 proof of concept | **any** windowed/borderless game (D3D9–12, OpenGL, Vulkan); no injection |

Both need a Vulkan-capable GPU driver (any NVIDIA / AMD / Intel driver from the
last few years). Neither has been validated on real hardware yet — see the
checklists in `windows-port.md` §3 (layer) and `windows-capture.md` §3
(capture). Please report what happens.

## Mode 2 — `bdex_capture.exe` (any game)

1. Put the game in **windowed or borderless** mode (exclusive fullscreen cannot
   be overlaid — same limitation as Lossless Scaling).
2. Open a terminal in this folder and run one of:
   ```
   bdex_capture.exe --list                      # which windows can be captured
   bdex_capture.exe --title "Kenshi" --fit      # capture by title, fill the monitor
   bdex_capture.exe --pid 1234 --scale 1.5      # capture by process id, 1.5x overlay
   bdex_capture.exe                             # alt-tab into the game within 5 s
   ```
3. An always-on-top overlay appears over (or, with `--fit`, centred on the
   monitor of) the game window, showing the captured image upscaled with the
   layer's bicubic filter (`--filter 0|1|2` = bilinear / bicubic / Lanczos).
   `--hud 2` prints `capture > output` fps in the corner; the console logs the
   same every 2 s.
4. **Ctrl+Alt+Q** quits, and so does closing the game.

What phase 1 validates: Windows.Graphics.Capture → D3D11 → Vulkan interop
(keyed mutex) → compute shader → presentation, in real time. Frame generation
itself (phase 2) is not wired into the capture path yet; the overlay shows the
game at its own frame rate.

Known limits of the PoC: keyboard input reaches the game (the overlay never
takes focus); mouse clicks pass through only where Windows honours
`WS_EX_TRANSPARENT` hit-testing — if the game stops reacting to the mouse,
run without `--fit` and click on a part of the game window the overlay does
not cover, or use `--scale` < 1. Set `BDEX_CAP_VALIDATION=1` to load the
Khronos validation layer when reporting a problem.

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
