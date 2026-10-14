# bdex-framegen — Decky Loader plugin

The Steam Deck's quick access menu (the « … » button) for the bdex-framegen
frame generation layer, in Game Mode, where the desktop control panel cannot
stay open beside the game:

- **Every game**: Vulkan and Proton games generate frames from their start
  (the panel's « Game Mode — without the panel », `require_app = 0`);
- **Frames per game frame**: Auto (up to the screen's refresh rate),
  Adaptive (every refresh, fractional ratios), ×2, ×3, ×4;
- **Mode**: interpolation, hybrid, extrapolation;
- **Paused**, **FPS counter**;
- **Settings for this game only**: the running game's own section of the
  configuration (off: back to the settings of every game);
- the running game's frame rates, as the layer reports them.

The plugin only edits `~/.config/bdex-framegen.conf` (the same lines as the
panel; game sections are kept); the layer reads it again by itself, so the
changes reach the running game within a second. The layer itself must be
installed (`install.sh` from the release archive, in Desktop Mode).

Build: `tools/ci/decky-check.sh` (Docker, Node 20, pnpm) writes
`dist/bdex-framegen-decky.zip`; install it with Decky's « Install plugin
from ZIP » (developer mode).
