# Windows compatibility — scope

> **Superseded as the Windows direction by [`windows-capture.md`](windows-capture.md).**
> This document scopes porting the *Vulkan layer* to Windows, which only reaches
> native-Vulkan games. The chosen direction is instead a **capture-based**
> product (Lossless-Scaling style) that is render-API agnostic. This file is kept
> for reference and for the reusable portability audit in §1–§2 (the config /
> process-name / `%APPDATA%` shims apply to the capture product too).

Scoping document for making **bdex-framegen** run on Windows, based on an audit
of the current tree (0.3.0 + unreleased). It states what already ports, the
concrete blockers with file references and a fix for each, a phased plan, and
the one decision that has to be made up front.

---

## 0. The decision that gates everything: Windows = Vulkan-only

**On Linux, this layer covers almost everything** because Proton runs Direct3D
games through DXVK / VKD3D-Proton, which translate D3D → Vulkan, and our
implicit Vulkan layer sits in that Vulkan path. So a D3D11/D3D12 game under
Proton *is* a Vulkan app from the layer's point of view.

**On native Windows that translation does not happen.** A D3D game talks to
Direct3D directly; there is no Vulkan swapchain for us to intercept. A Vulkan
implicit layer on Windows therefore only affects games that render with
**Vulkan natively**. That is a real but much smaller set, e.g. Doom (2016) /
Doom Eternal, Wolfenstein II/Youngblood, Red Dead Redemption 2 (Vulkan mode),
No Man's Sky (Vulkan), Rainbow Six Siege (Vulkan), Baldur's Gate 3 (Vulkan),
Ghost of Tsushima, id-Tech / Source 2 titles, and emulators that use Vulkan
(yuzu/ryujinx-likes, RPCS3, PCSX2, Dolphin, Cemu…).

Consequences to decide before spending effort:

- **A Vulkan layer will not make the average D3D-only Windows game work.**
  Selling it as "works like Lossless Scaling on Windows" would be misleading —
  Lossless Scaling captures the *window*, independent of the render API.
- Covering D3D games on Windows the LS way would need a **completely different
  mechanism** (DXGI/D3D present hooking or desktop-duplication capture) — a
  separate product, not this layer. Out of scope here; noted as an alternative.
- A middle path exists: users can run a D3D game through **DXVK on Windows**
  (dxvk `.dll` overrides), which *would* expose it to us — but that is niche
  and per-game fiddly, not a mainstream story.

**Recommendation:** port the layer to Windows for **native-Vulkan games and
emulators**, market it honestly as such, and keep the D3D-capture idea as a
possible separate track. Everything below assumes that decision.

---

## 1. What already ports (little to no work)

The audit is encouraging — the engine is mostly platform-agnostic:

- **The whole frame-generation / swapchain-virtualisation core** is WSI-agnostic
  Vulkan. It never touches X11/Wayland/Win32 directly; it works on
  `VkSurfaceKHR` and `VkSwapchainKHR` abstractly.
- **Threading** is `std::thread` / `std::mutex` / `std::condition_variable`
  (`swapchain.*`, `device.*`) — portable as-is.
- **Calling convention** is already correct: every entry point uses
  `VKAPI_ATTR`/`VKAPI_CALL`, which the Vulkan headers map to `__stdcall` on
  Windows. No changes needed there.
- **Logging** (`log.cpp`) is `FILE*` + `fprintf` + `std::atomic`/`std::chrono` —
  portable.
- **Shaders**: compiled with `glslc`, which ships in the LunarG Vulkan SDK for
  Windows; the embed step (`cmake/EmbedShader.cmake`) just runs it.
- **Upscaling works on Windows via the existing X11-style path.** `VK_KHR_win32_
  surface` reports a fixed `currentExtent` (the window size), exactly like X11 —
  so `applyRenderScale()` in `layer.cpp` reduces it directly and the
  native-Wayland throwaway-swapchain dance is not needed. (The Wayland code path
  simply never triggers on Windows.)
- **The negotiate/enable model**: removing `enable_environment` and gating on
  the config file works identically on Windows.

---

## 2. Concrete blockers and their fixes

### 2.1 Build system (`CMakeLists.txt`, `layer/CMakeLists.txt`)

| Item | Current (Linux) | Windows fix |
|---|---|---|
| Warning/visibility flags | `-Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden` (GCC/Clang) | Wrap in `if(MSVC) ... /W4 ... else() ...`; `-fvisibility=hidden` has no MSVC equivalent (symbols are hidden by default, exported via `__declspec`/`.def`). |
| Symbol export | `LINK_FLAGS "-Wl,--version-script=layer.map -Wl,-Bsymbolic"` | GNU-ld only. On Windows export the 3 entry points via a `.def` file (`EXPORTS vkGetInstanceProcAddr …`) or `__declspec(dllexport)`. Drop `-Bsymbolic` (it guards against libvulkan symbol interposition — a Linux-`dlopen` concern that doesn't exist on Windows). |
| Output name | `PREFIX libVkLayer_`, suffix `.so` | Produce `VkLayer_bdex_framegen.dll`. The loader finds it via the manifest's `library_path`, so the exact name is free; keep it descriptive. |
| 32-bit | `-m32` multilib flags | MSVC: a second configure with `-A Win32` (vs `-A x64`); MinGW: a separate i686 toolchain. Same "two builds, two manifests" shape as today, different switches. Register the 32-bit manifest under `WOW6432Node` (see 2.3). |
| Vulkan / glslc | `find_package(Vulkan)`, `find_program(glslc)` | Both provided by the LunarG SDK on Windows; works unchanged. |
| Demo | GLFW via `pkg_check_modules` | Not needed for the port. Gate it off on Windows, or wire GLFW via vcpkg later. |

### 2.2 Source portability (a handful of `#ifdef _WIN32` shims)

- **Export macro** — `layer.cpp:724`
  `#define BDEX_EXPORT extern "C" __attribute__((visibility("default")))`
  → `#ifdef _WIN32: extern "C" __declspec(dllexport)` (or rely on the `.def`).
- **printf format attribute** — `log.h:12`
  `__attribute__((format(printf, 2, 3)))` → guard behind `#if defined(__GNUC__)`,
  empty on MSVC.
- **Process names for `[game]` sections** — `config.cpp:207` reads
  `/proc/self/cmdline`. Windows: implement `processNames()` with
  `GetModuleFileNameW(NULL, …)` (the exe path) and/or `GetCommandLineW`, then the
  existing `baseName()` logic. This is what makes per-game profiles work on
  Windows, so it matters.
- **Environment enumeration** — `config.cpp:10,253` uses `extern char** environ`.
  Windows has `_environ` (or `GetEnvironmentStringsW`); wrap in a small helper.
  `getenv("BDEX_FG…")` reads are already portable.
- **Config file location** — `config.cpp:275-276` uses `XDG_CONFIG_HOME` / `HOME`
  → on Windows use `%APPDATA%\bdex-framegen\bdex-framegen.conf`
  (`SHGetKnownFolderPath(FOLDERID_RoamingAppData)` or `getenv("APPDATA")`). Keep
  `BDEX_FG_CONFIG` as the override on both.

None of these touch the hot path; they are startup/config code. Estimate: a
single `platform.{h,cpp}` with three or four functions.

### 2.3 Installation — registry instead of a directory

This is the biggest *new* piece. Linux discovers implicit layers by dropping a
JSON in `…/vulkan/implicit_layer.d`. **Windows discovers them via the registry.**

- Register the manifest by adding a value under
  `HKEY_CURRENT_USER\SOFTWARE\Khronos\Vulkan\ImplicitLayers` (per-user, no admin)
  — value **name** = absolute path to the JSON, value **type** = `REG_DWORD`,
  **data** = `0` (0 means "enabled"). System-wide is the same under
  `HKEY_LOCAL_MACHINE` (needs admin). 32-bit layers live under
  `…\WOW6432Node\Khronos\Vulkan\ImplicitLayers`.
- The manifest `library_path` (`bdex_framegen.json.in`) should be `.\\VkLayer_
  bdex_framegen.dll` relative to the JSON (keep DLL + JSON in the same folder),
  or an absolute path with escaped backslashes. The `configure_file` step just
  needs a Windows branch for the path.
- **Deliverable:** a `register-layer.ps1` / `unregister-layer.ps1` PowerShell
  pair (per-user, no admin) that copies `VkLayer_bdex_framegen.dll` +
  `VkLayer_bdex_framegen.json` into e.g. `%LOCALAPPDATA%\bdex-framegen\` and
  writes/removes the registry value. This is the Windows equivalent of
  `tools/install.sh` / `uninstall.sh`.
- Later: wrap it in a proper installer (Inno Setup or NSIS) that also handles
  both bitnesses and offers per-user vs all-users.

### 2.4 GUI

`tools/bdex-framegen-gui` is Python + GTK4/libadwaita — Linux-only, and there is
no `.desktop` concept on Windows. Options, cheapest first:

1. **Ship no GUI initially.** The layer reads the config file directly, so a
   documented `%APPDATA%\bdex-framegen\bdex-framegen.conf` + the register script
   is a working v1. The control-panel model (global/per-game, all keys) is just
   an INI file.
2. **Small native config tool** later (a tray app / simple window). Candidates:
   a tiny C++/Win32 or a Python app with a Windows-friendly toolkit. Reusing the
   GTK panel on Windows is possible (GTK4 runs on Windows) but the packaging is
   heavy; not worth it for v1.
3. **Cross-platform rewrite** of the panel (e.g. a small web-UI-in-a-window, or
   Dear ImGui) if Windows becomes a first-class target — a deliberate later
   decision, not part of the port.

### 2.5 Tests / CI

- The **unit tests** (`tests/test_*.cpp`, `test.h`) are portable C++ — they
  should build and run under MSVC with a CMake tweak.
- The **integration test** (`tests/integration.sh`) is bash + `VK_ADD_IMPLICIT_
  LAYER_PATH` + the demo; on Windows it needs a PowerShell equivalent, or gets
  skipped on Windows CI initially.
- Add a **Windows CI job** (GitHub Actions `windows-latest`) that installs the
  Vulkan SDK, builds x64 (+ x86), and runs the unit tests. Prebuilt release
  artifacts (a zip: DLLs + JSONs + register scripts) come from here.
- **Code signing:** unsigned DLLs trip SmartScreen/AV. For distribution, plan an
  Authenticode signing step (optional for early testers).

---

## 3. Phased plan

**Phase 0 — decision (no code).** Confirm the Vulkan-only scope in §0 is
acceptable, and that the target is native-Vulkan games + emulators. If the real
goal is "any Windows game like Lossless Scaling", stop here — that needs a
D3D/DXGI-capture product instead.

**Phase 1 — the layer builds and loads on Windows.**
- `platform.{h,cpp}` shims: export macro, printf attribute, `processNames()`,
  environment enumeration, config path (§2.2).
- CMake: MSVC branch, `.def` export, drop `-Bsymbolic`/version-script on Windows,
  `.dll` output, Windows `library_path` (§2.1).
- `register-layer.ps1` / `unregister-layer.ps1` (§2.3), x64 first.
- **Acceptance:** `vulkaninfo` shows the layer loaded; a native-Vulkan title
  (or vkcube / the demo built on Windows) shows frame generation working.

**Phase 2 — parity and packaging.**
- Verify **upscaling** on Windows (should work via the win32 fixed-extent path —
  §1); confirm the HUD and present-mode handling.
- 32-bit build + `WOW6432Node` registration (many older Vulkan games/emulators
  are 32-bit).
- Release zip + optional Inno/NSIS installer; Windows CI + unit tests.

**Phase 3 — usability.**
- A Windows config tool (§2.4 option 2), or a documented config-file workflow.
- Optional Authenticode signing.
- Docs: a Windows section in the README.

---

## 4. Risks & unknowns

- **Value/coverage** (the §0 caveat) is the dominant risk, not the engineering.
- **Present-path compatibility** with real Windows Vulkan games and emulators is
  unverified; the layer virtualises the swapchain, and some engines/anti-cheat
  may react to an implicit layer (a few anti-cheats block unknown layers).
- **Anti-cheat**: an implicit layer injecting into a protected process can be
  flagged. Multiplayer titles are a likely no-go; single-player/emulators are
  the safe audience.
- **Testing**: no Windows machine is available in this environment — Phase 1
  needs a real Windows box (or a CI runner + a VM) to validate loading and a
  game.
- **Fractional/DPI scaling** on Windows may interact with the render-scale hook;
  worth an explicit test.

---

## 5. Effort estimate (rough)

| Phase | Work | Size |
|---|---|---|
| 1 | platform shims + CMake MSVC + register script + first load | ~2–4 days incl. debugging on a real box |
| 2 | upscaling/HUD verification, 32-bit, packaging, CI | ~2–4 days |
| 3 | config tool + signing + docs | open-ended (depends on GUI ambition) |

The **layer core is portable**; the real cost is the Windows loader/registry
plumbing, packaging, and *testing on actual hardware with real Vulkan games* —
not rewriting the engine.

---

## 6. Out of scope (explicitly)

- Making D3D-only Windows games work (needs a separate DXGI/D3D-capture design).
- A cross-platform GUI rewrite (tracked as a Phase-3 decision, not a given).
- Console/Mac (MoltenVK is a different WSI story again).
