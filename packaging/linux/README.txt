bdex-framegen for Linux (x86_64) — prebuilt
===========================================

Frame generation (optical flow, x2-x4) and upscaling for Vulkan games, as an
implicit Vulkan layer: native Vulkan games, and Direct3D games under Proton
(DXVK / VKD3D-Proton). Games protected by EasyAntiCheat, BattlEye and the like
are left alone automatically.

Install (per user, into ~/.local; no root, nothing to compile):
    ./install.sh
Remove:
    ./uninstall.sh

Then turn it on, globally or per game:
    bdex-framegen-gui                      the control panel (GTK 4)
    BDEX_FG=1 %command%                    Steam launch options, one game
    bdex-framegen --preset latency -- <game>

Modes: interpolation (default: the smoothest, holds each real frame half a
game frame) or extrapolation (preset "latency": ~0 ms added hold, may show
artefacts on abrupt motion). Settings live in ~/.config/bdex-framegen.conf.

Contents: 64-bit layer (lib/), 32-bit layer for 32-bit games (lib32/, when
present), the bdex-framegen command-line launcher, the control panel, per-game
profiles. The layer carries its own C++ runtime (works inside the Steam
runtime); it needs glibc 2.35 or newer.

Project: https://github.com/Bdexez/bdex_frame_gen
