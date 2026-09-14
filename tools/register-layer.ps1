# Registers the bdex-framegen Vulkan layer for the current Windows user, no
# admin rights needed. The loader discovers implicit layers through the
# registry (not a directory like on Linux):
#
#   HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers   (per-user)
#   HKCU\SOFTWARE\WOW6432Node\...                 (32-bit layer view)
#
# The DLL and manifest are copied to %LOCALAPPDATA%\bdex-framegen so the layer
# keeps working even if the build tree is deleted. The manifest uses a
# relative library_path, so DLL and .json must stay in the same folder.
#
# Prerequisites: Visual Studio 2022 (C++ workload) and the Vulkan SDK
# (provides the headers + glslc).
#
# Usage (x64, from the repo root):
#   cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64
#   cmake --build build-win --config Release
#   powershell -File tools\register-layer.ps1 -Dll build-win\layer\Release\VkLayer_bdex_framegen.dll
#
# 32-bit (old games / emulators): same with -A Win32, then:
#   powershell -File tools\register-layer.ps1 -Dll build-win32\layer\Release\VkLayer_bdex_framegen.dll -Wow6432
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, HelpMessage = "Path to VkLayer_bdex_framegen.dll")]
    [string] $Dll,
    [switch] $Wow6432,
    [string] $LayerName = 'VK_LAYER_BDEX_framegen'
)
$ErrorActionPreference = 'Stop'

$dllPath = (Resolve-Path -LiteralPath $Dll).Path
if ($Wow6432) { $LayerName = "${LayerName}_32" }

$dest = Join-Path $env:LOCALAPPDATA $(if ($Wow6432) { 'bdex-framegen32' } else { 'bdex-framegen' })
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$dllDest = Join-Path $dest 'VkLayer_bdex_framegen.dll'
Copy-Item -LiteralPath $dllPath -Destination $dllDest -Force

$json = Join-Path $dest "$LayerName.json"
@"
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "$LayerName",
    "type": "GLOBAL",
    "library_path": ".\\VkLayer_bdex_framegen.dll",
    "api_version": "1.4.0",
    "implementation_version": "1",
    "description": "BDEX frame generation: interpolates extra frames between the game's frames",
    "functions": {
      "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
      "vkGetDeviceProcAddr": "vkGetDeviceProcAddr",
      "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
    },
    "disable_environment": { "BDEX_FG_DISABLE": "1" }
  }
}
"@ | Set-Content -Path $json -Encoding ASCII

$keyPath = 'HKCU:\SOFTWARE\Khronos\Vulkan\ImplicitLayers'
if ($Wow6432) { $keyPath = 'HKCU:\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers' }
New-Item -Path $keyPath -Force | Out-Null
New-ItemProperty -Path $keyPath -Name $json -PropertyType DWord -Value 0 -Force | Out-Null

Write-Host "Registered $LayerName for the current user."
Write-Host "  DLL      : $dllDest"
Write-Host "  Manifest : $json"
Write-Host "  Registry : $keyPath"
Write-Host ''
Write-Host 'Verify with:  vulkaninfo --summary    (should list VK_LAYER_BDEX_framegen)'
Write-Host 'Config file:  %APPDATA%\bdex-framegen\bdex-framegen.conf   (enabled=1 to turn on)'
Write-Host 'Logs: stderr by default; set log_file in the config, e.g. log_file=%TEMP%\bdex-fg.log'
Write-Host 'Unload without uninstalling: set BDEX_FG_DISABLE=1 in the environment.'
