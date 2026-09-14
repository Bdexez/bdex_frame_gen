# Removes the bdex-framegen layer registration created by register-layer.ps1
# (current user only; a system-wide install would live under HKLM and is not
# touched here).
#
# Usage:
#   powershell -File tools\unregister-layer.ps1            # 64-bit layer
#   powershell -File tools\unregister-layer.ps1 -Wow6432   # 32-bit layer
#   ... -RemoveFiles   also delete the copied DLL/manifest/config folder
[CmdletBinding()]
param(
    [switch] $Wow6432,
    [switch] $RemoveFiles,
    [string] $LayerName = 'VK_LAYER_BDEX_framegen'
)
$ErrorActionPreference = 'Stop'

if ($Wow6432) { $LayerName = "${LayerName}_32" }

$dest = Join-Path $env:LOCALAPPDATA $(if ($Wow6432) { 'bdex-framegen32' } else { 'bdex-framegen' })
$json = Join-Path $dest "$LayerName.json"

$keyPath = 'HKCU:\SOFTWARE\Khronos\Vulkan\ImplicitLayers'
if ($Wow6432) { $keyPath = 'HKCU:\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers' }

if (Test-Path $keyPath) {
    $prop = Get-ItemProperty -Path $keyPath -Name $json -ErrorAction SilentlyContinue
    if ($prop) {
        Remove-ItemProperty -Path $keyPath -Name $json
        Write-Host "Removed registry value: $keyPath -> $json"
    } else {
        Write-Host "Not registered: $json"
    }
} else {
    Write-Host "Not registered: $keyPath does not exist"
}

if ($RemoveFiles) {
    if (Test-Path $dest) {
        Remove-Item -Recurse -Force -Path $dest
        Write-Host "Deleted $dest"
    }
}
