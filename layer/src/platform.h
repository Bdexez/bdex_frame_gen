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

} // namespace bdex
