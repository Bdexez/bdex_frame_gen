bdex-framegen for Linux — prebuilt
==================================

Frame generation (optical flow, x2 to x8 or adaptive) and upscaling for Vulkan games, as an
implicit Vulkan layer: native Vulkan games, and Direct3D games under Proton
(DXVK / VKD3D-Proton). Games protected by EasyAntiCheat, BattlEye and the like
are left alone automatically.

Install (per user, into ~/.local; no root, nothing to compile):
    ./install.sh
Remove:
    ./uninstall.sh

Then, like the Windows launcher:
    1. launch your game normally (Steam...);
    2. open the control panel: bdex-framegen-gui (application menu:
       "bdex-framegen");
    3. « Actualiser la liste », pick the game in « Jeux détectés », set the
       options, « Démarrer ». « Arrêter » or closing the panel stops it.
Nothing generates while the panel is closed. Without the panel, per game:
    BDEX_FG=1 %command%                    Steam launch options
    bdex-framegen --preset latency -- <game>
Upscaling (render scale) only helps native Vulkan games started while the
panel is open; for Proton games lower the game's own resolution and upscale
with gamescope (-F fsr) or the game's FSR/DLSS.

Steam Deck / SteamOS: install in Desktop Mode (Konsole: ./install.sh). Game
Mode runs one application at a time, so the panel cannot stay open beside a
game there: in the panel, « Mode Jeu — sans le panneau » (all games, or the
selected one), or `bdex-framegen --always-on [GAME]`, or the launch option
BDEX_FG=1 %command%. Steam's framerate limit (Quick Access Menu) also caps the
generated frames: set it to Off (or the refresh rate) and cap the game itself
(fps_limit = 30 for 60 at x2) to generate above it.

Modes: interpolation (default: the smoothest, holds each real frame half a
game frame), extrapolation (preset "latency": ~0 ms added hold, may show
artefacts on abrupt motion) or hybrid (x3 and up: one interpolated frame, the
real one, then predictions). Settings live in ~/.config/bdex-framegen.conf.

Contents: the layer (lib/; x86_64 archives also have the 32-bit one for
32-bit games, lib32/), the bdex-framegen command-line launcher, the control
panel, per-game profiles. The layer carries its own C++ runtime (works inside
the Steam runtime).

Requirements: glibc 2.29 (x86_64) / 2.30 (aarch64) or newer — Ubuntu 20.04+,
Debian 11+, Mint 20+, Fedora, Arch / SteamOS, openSUSE Leap 15.3+, RHEL 9...;
the control panel needs Python 3 with PyGObject and GTK 4 — libadwaita 1.4+
is used when present, not required (SteamOS has none). The archive's
architecture (share/bdex-framegen/ARCH) must be the machine's.

Steam, Heroic, Lutris or Bottles installed as a Flatpak: install.sh also
installs the layer inside their sandbox (~/.var/app/<app>/data); the control
panel keeps their copy of the settings up to date. Install them (and run them
once) before install.sh, or run it again after. Snap packages are not
supported.

Project: https://github.com/Bdexez/bdex_frame_gen
