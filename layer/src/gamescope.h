#pragma once
// gamescope — the compositor of Steam's Game Mode (Steam Deck, SteamOS) and
// of gamescope sessions elsewhere: what a Vulkan program under it can learn
// about the display.
//
// Its WSI layer (VkLayer_FROG_gamescope_wsi, below this one) presents our
// real swapchain to gamescope and answers vkGetRefreshCycleDurationGOOGLE
// itself, from what gamescope sends: the display's refresh cycle, or with
// Steam's framerate limit on (the Quick Access Menu), the limit's. gamescope
// then shows at most one frame of the game per such cycle: generated frames
// past it hold the game back (measured: 30 fps game at x2 under a 30 fps
// limit ran at 15 fps, 233 ms from present to screen).
namespace bdex {

// This process presents through gamescope's WSI layer: gamescope runs it
// (GAMESCOPE_WAYLAND_DISPLAY) and the layer is enabled (ENABLE_GAMESCOPE_WSI,
// which gamescope sets for its children, and no DISABLE_GAMESCOPE_WSI).
bool gamescopeWsi();

// Steam's framerate limit is on. gamescope writes its limiter state to the
// file named by GAMESCOPE_LIMITER_FILE (a 32-bit 0 / 1) for the clients
// without its Wayland limiter protocol; false without one.
bool gamescopeLimiterActive();

} // namespace bdex
