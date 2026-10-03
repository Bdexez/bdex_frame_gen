// Injection on Windows (03/10, the user's choice: injection by default,
// capture for games with an anti-cheat): the Vulkan layer runs inside the
// game's own presentation — no frame of latency added, where capture adds
// one. It reaches Vulkan games directly, and Direct3D 9/10/11/12 games
// through the translation layers Proton uses, copied next to the game's
// executable: DXVK (D3D9/10/11) and VKD3D-Proton with DXVK's dxgi (D3D12).
// The game then presents through Vulkan, and the layer arms it like any
// Vulkan game the next time it starts.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace bdex {

// What the launcher knows of a running game.
struct GameInfo {
    DWORD pid = 0;
    std::wstring exePath;  // its executable's full path ("" when the process cannot be opened)
    std::string name;      // normalised name (lower case, no extension): its [section]
    bool is64 = true;      // from the executable's PE header
    enum Api { Unknown, Vulkan, D3D9, D3D10, D3D11, D3D12, OpenGL } api = Unknown;
    bool translated = false;          // our DXVK / VKD3D-Proton files are next to its executable
    std::string antiCheat;            // an anti-cheat found with it ("" none)
    bool antiCheatException = false;  // injection kept despite it (platformAntiCheatException: Rocket League)
};

GameInfo inspectGame(DWORD pid);
const char* apiName(GameInfo::Api api);

// The DLLs `api` needs, by file name (empty: no translation, Vulkan / OpenGL).
std::vector<std::wstring> translationFiles(GameInfo::Api api);
// They are shipped next to bdex_capture.exe (dxvk\x64, vkd3d-proton\x64...).
bool translationAvailable(const GameInfo& game);
// Copies them next to the game's executable — never over a file that is
// already there (another translation layer, ReShade...): `why` then says
// which — and lists them in bdex-framegen-injection.txt there and in the
// launcher's own list (injectedGames) so they can be removed.
bool installTranslation(const GameInfo& game, std::string& why);
// Removes what installTranslation put next to `exePath` (the game closed:
// a loaded DLL cannot be deleted). False with `why` otherwise.
bool removeTranslation(const std::wstring& exePath, std::string& why);
// Every executable installTranslation prepared (%LOCALAPPDATA%\bdex-framegen\injections.txt).
std::vector<std::wstring> injectedGames();

// Registers the Vulkan layer for the current user (HKCU, as
// register-layer.ps1 does): the DLL and the profiles copied to
// %LOCALAPPDATA%\bdex-framegen, its manifest beside them; the 32-bit layer
// too when the package has it (x86\VkLayer_bdex_framegen.dll). Updates the
// copy when the package's DLL changed (not while a game holds it loaded).
bool registerLayerForUser(std::string& why);

// The warning shown before frame generation starts on a game with an
// anti-cheat (the launch goes ahead): what the user risks, by method.
std::wstring antiCheatWarning(const GameInfo& game, bool injection);

} // namespace bdex
