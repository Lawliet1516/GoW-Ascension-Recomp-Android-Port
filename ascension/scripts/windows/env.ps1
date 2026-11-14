# Build settings from .env in the repository root (local, never committed).
# Dot-source it from the other scripts:
#
#   . (Join-Path $PSScriptRoot "env.ps1")
#
# Sets $Root (ascension/) and $Cfg (paths made absolute). Stops when .env or a
# setting the build needs is missing.

$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$envFile = Join-Path $Root ".env"
if (-not (Test-Path $envFile)) { Write-Host "missing $envFile"; exit 1 }
$dotenv = @{}
foreach ($line in Get-Content $envFile) {
    $l = $line.Trim()
    if (-not $l -or $l.StartsWith("#")) { continue }
    $eq = $l.IndexOf("=")
    if ($eq -lt 1) { continue }
    $dotenv[$l.Substring(0, $eq).Trim()] = $l.Substring($eq + 1).Trim().Trim('"').Trim("'")
}

function Get-Setting([string]$Name, [switch]$Required) {
    $v = $dotenv[$Name]
    if ($Required -and -not $v) { Write-Host "missing $Name in $envFile"; exit 1 }
    return $v
}
function Resolve-Setting([string]$Path) {
    if (-not $Path) { return $Path }
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

$Cfg = @{}
$Cfg.PS3KIT_DIR         = Resolve-Setting (Get-Setting "PS3KIT_DIR" -Required)
$Cfg.WORK_DIR           = Resolve-Setting (Get-Setting "WORK_DIR" -Required)
$Cfg.VCVARS             = Get-Setting "VCVARS" -Required
$Cfg.FFMPEG_INCLUDE_DIR = Resolve-Setting (Get-Setting "FFMPEG_INCLUDE_DIR" -Required)
$Cfg.BUILD_TYPE         = Get-Setting "BUILD_TYPE" -Required
$Cfg.JOBS               = Get-Setting "JOBS"
$Cfg.SPU_FMA            = (Get-Setting "SPU_FMA") -eq "1"
$Cfg.SPU_XFLOAT_APPROX  = (Get-Setting "SPU_XFLOAT_APPROX") -ne "0"   # on unless .env says 0
$Cfg.SPU_LS_HOOKS       = (Get-Setting "SPU_LS_HOOKS") -eq "1"        # LS diagnostics in lifted code
$Cfg.SPU_CODEGEN2       = (Get-Setting "SPU_CODEGEN2") -eq "1"        # faster lifted SPU helpers (ASCENSION_SPU_CODEGEN2)
# Optional: SDL2's VC package cmake\ folder (PlayStation controllers through
# SDL2) and FFmpeg's bin\ folder; their DLLs are copied beside the game.
$Cfg.SDL2_DIR           = Resolve-Setting (Get-Setting "SDL2_DIR")
$Cfg.FFMPEG_BIN_DIR     = Resolve-Setting (Get-Setting "FFMPEG_BIN_DIR")
# Optional: a merged clang profile (scripts\windows\pgo-train.ps1) the build
# optimises with (profile-guided optimisation).
$Cfg.PGO_PROFILE        = Resolve-Setting (Get-Setting "PGO_PROFILE")
$vk = Get-Setting "VULKAN_SDK"
if ($vk) { $env:VULKAN_SDK = $vk }
$Cfg.VULKAN_SDK = $vk
# Optional: a standalone LLVM bin\ folder (clang-cl, lld-link, llvm-lib), put
# first on PATH for this PowerShell session and the cmd it starts. Used when
# Visual Studio's own Clang component is not installed.
$llvm = Get-Setting "LLVM_BIN"
if ($llvm -and (Test-Path $llvm)) { $env:PATH = "$llvm;$env:PATH" }
$Cfg.LLVM_BIN = $llvm
foreach ($k in "PS3KIT_DIR", "WORK_DIR", "VCVARS") {
    if (-not (Test-Path $Cfg[$k])) { Write-Host "$k not found: $($Cfg[$k])"; exit 1 }
}
