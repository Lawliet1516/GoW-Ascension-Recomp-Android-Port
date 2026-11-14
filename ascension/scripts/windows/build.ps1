# Build AscensionRecomp.exe (ninja in build\win) and log it. The settings
# come from .env (scripts\windows\env.ps1).
#
#   .\scripts\windows\build.ps1                 # log: build\logs\build.log
#   .\scripts\windows\build.ps1 -Name sse41     # log: build\logs\build-sse41.log
#   .\scripts\windows\build.ps1 -Configure      # run CMake again with .env's settings first
#
# CMake runs by itself the first time (no build\win\build.ninja). Prints how
# long it took, the error lines if any, and the last line of the log; the
# exit code is ninja's.
param([string]$Name = "", [switch]$Configure)

. (Join-Path $PSScriptRoot "env.ps1")
$Build = Join-Path $Root "build\win"
$Logs  = Join-Path $Root "build\logs"
New-Item -ItemType Directory -Force $Logs | Out-Null
$Log = Join-Path $Logs ($(if ($Name) { "build-$Name.log" } else { "build.log" }))

if ($Configure -or -not (Test-Path (Join-Path $Build "build.ninja"))) {
    $onoff = { param($b) if ($b) { "ON" } else { "OFF" } }
    $cmakeArgs = @(
        "-S", "`"$Root`"", "-B", "`"$Build`"", "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$($Cfg.BUILD_TYPE)",
        "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
        "-DPS3RECOMP_DIR=`"$($Cfg.PS3KIT_DIR)`"",
        "-DPS3RECOMP_FFMPEG_RUNTIME=ON", "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=`"$($Cfg.FFMPEG_INCLUDE_DIR)`"",
        "-DASCENSION_SPU_FMA=$(& $onoff $Cfg.SPU_FMA)",
        "-DASCENSION_SPU_XFLOAT_APPROX=$(& $onoff $Cfg.SPU_XFLOAT_APPROX)",
        "-DASCENSION_SPU_LS_HOOKS=$(& $onoff $Cfg.SPU_LS_HOOKS)",
        "-DASCENSION_SPU_CODEGEN2=$(& $onoff $Cfg.SPU_CODEGEN2)"
    )
    if ($Cfg.SDL2_DIR) { $cmakeArgs += @("-DPS3RECOMP_PAD_SDL2=ON", "-DSDL2_DIR=`"$($Cfg.SDL2_DIR)`"") }
    if ($Cfg.PGO_PROFILE -and (Test-Path $Cfg.PGO_PROFILE)) {
        $cmakeArgs += @("-DASCENSION_PGO=USE", "-DASCENSION_PGO_PROFILE=`"$($Cfg.PGO_PROFILE)`"")
    } else { $cmakeArgs += @("-DASCENSION_PGO=") }
    $cmakeArgs = $cmakeArgs -join " "
    $ConfLog = Join-Path $Logs "configure.log"
    Write-Host "configuring build\win (log: $ConfLog)"
    cmd /c "`"$($Cfg.VCVARS)`" >nul && cmake $cmakeArgs" 2>&1 | Out-File -Encoding utf8 $ConfLog
    if ($LASTEXITCODE -ne 0) { Get-Content $ConfLog -Tail 20; exit $LASTEXITCODE }
}

$jobs = if ($Cfg.JOBS) { "-j $($Cfg.JOBS)" } else { "" }
$sw = [Diagnostics.Stopwatch]::StartNew()
cmd /c "`"$($Cfg.VCVARS)`" >nul && ninja -C `"$Build`" $jobs" 2>&1 | Out-File -Encoding utf8 $Log
$code = $LASTEXITCODE
$sw.Stop()

# Run-time DLLs beside the game: SDL2 (controllers) and FFmpeg (movies).
if ($code -eq 0) {
    if ($Cfg.SDL2_DIR) {
        $sdlDll = Join-Path (Split-Path -Parent $Cfg.SDL2_DIR) "lib\x64\SDL2.dll"
        if (Test-Path $sdlDll) { Copy-Item $sdlDll $Build -Force }
    }
    if ($Cfg.FFMPEG_BIN_DIR -and (Test-Path $Cfg.FFMPEG_BIN_DIR)) {
        Get-ChildItem (Join-Path $Cfg.FFMPEG_BIN_DIR "*.dll") | Copy-Item -Destination $Build -Force
    }
}

$errors = Select-String -Path $Log -Pattern "error:|FAILED:|fatal error|LNK\d{4}" | Select-Object -First 15
Write-Host ("build {0} in {1:mm\:ss}  (log: {2})" -f $(if ($code -eq 0) { "OK" } else { "FAILED" }), $sw.Elapsed, $Log)
if ($errors) { $errors | ForEach-Object { Write-Host "  $($_.Line)" } }
Write-Host ("last line: " + (Get-Content $Log -Tail 1))
exit $code
