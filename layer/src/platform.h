#pragma once
#include <string>
#include <vector>

namespace bdex {

// Executable names of the current process, lower-cased, without directory or
// extension ("BEService64.exe" -> "beservice64"); used for [game] config
// sections and the anti-cheat check.
std::vector<std::string> platformProcessNames();

// The process environment as a NULL-terminated list of "KEY=VALUE" strings
// (POSIX `environ` / MSVCRT `_environ`).
char** platformEnviron();

// Full path of the user config file (Linux: $XDG_CONFIG_HOME or
// ~/.config; Windows: %APPDATA%\bdex-framegen), or "" when no location
// is known. The BDEX_FG_CONFIG override is handled by the caller.
std::string platformConfigPath();
// Per-game profiles shipped with the build (loaded before the user's file);
// BDEX_FG_PROFILES overrides the location, an empty value disables them.
std::string platformBundledProfilesPath();

// Expands environment references in a path the user wrote in the config file,
// so e.g. `log_file=%TEMP%\bdex-fg.log` resolves to a real path instead of
// being opened verbatim. Windows expands `%VAR%` (via the OS); Linux expands
// `$VAR`, `${VAR}` and a leading `~`. Unknown variables are left as-is, and a
// string with nothing to expand is returned unchanged.
std::string platformExpandPath(const std::string& in);

// True when a known multiplayer anti-cheat process is running, in which
// case the caller must leave the layer disabled for this process. Fills
// `foundName` (optional) with the matching process name.
bool platformAntiCheatPresent(std::string* foundName);

// True for a competitive multiplayer game's executable name (normalised:
// lower case, no directory or extension) — games whose anti-cheat has no
// process to detect (VAC, Vanguard, Ricochet...) or runs inside the game.
bool platformIsCompetitiveGame(const std::string& name);

// The control panel's session (require_app), like the Windows launcher: while
// it is open it keeps <platformStatusDir()>/app.session = "<its pid> <target
// pid> <refresh Hz>", the target being the game where « Démarrer » started the generation
// (0 = none), rewritten every second: older than 5 s, the panel is gone.
struct AppSession {
    bool open = false;      // the panel is running
    long target = 0;        // process generating frames, 0 = none
    double displayHz = 0.0; // the monitor's refresh rate as the panel reads it, 0 unknown
};
AppSession platformAppSession();

// Vulkan programs that are not games (browsers, Steam, compositors...): with
// require_app the layer arms every other Vulkan program, never these.
bool platformIsNonGame(const std::string& name);
// The current executable's name, normalised like platformProcessNames (its
// command line can be rewritten: Electron / Chromium put it in one string).
std::string platformExeName();
// This process is a desktop application that draws with Vulkan (GTK 4).
bool platformIsDesktopApp();

// Directory of the per-process live status files (status.h): Linux
// ~/.cache/bdex-framegen/run (shared with Steam's container; else
// /tmp/bdex-framegen-<uid>), Windows
// %TEMP%\bdex-framegen; BDEX_FG_STATUS_DIR overrides it, an empty value
// disables the files. "" when no location is known.
std::string platformStatusDir();
// Directory for small persistent caches (online_tune decisions): Linux
// $XDG_CACHE_HOME/bdex-framegen (else ~/.cache/bdex-framegen), Windows
// %LOCALAPPDATA%\bdex-framegen; BDEX_FG_CACHE_DIR overrides it, an empty
// value disables the cache. "" when no location is known.
std::string platformCacheDir();
long platformPid();

} // namespace bdex
