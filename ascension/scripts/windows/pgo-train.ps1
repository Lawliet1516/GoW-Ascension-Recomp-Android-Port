# Profile-guided optimisation, training run: plays the instrumented build
# (build\pgo-gen, ASCENSION_PGO=GEN) into the first level and through combat,
# then closes its window so it writes its profile (build\pgo\*.profraw).
#
#   .\scripts\windows\pgo-train.ps1 [-Seconds 200] [-Slow 1.5]
#
# -Slow stretches the menu button presses for the slower instrumented build.
# Then: llvm-profdata merge -o build\pgo\game.profdata build\pgo\*.profraw
# and a build with -DASCENSION_PGO=USE -DASCENSION_PGO_PROFILE=<that file>.
param([int]$Seconds = 200, [double]$Slow = 1.5)
. (Join-Path $PSScriptRoot "env.ps1")
$Work = $Cfg.WORK_DIR
$Exe  = Join-Path $Root "build\pgo-gen\AscensionRecomp.exe"
$Elf  = Join-Path $Work "disc\PS3_GAME\USRDIR\EBOOT.ELF"
$Out  = Join-Path $Root "build\pgo"
$Log  = Join-Path $Root "build\logs\pgo-train.log"
$SaveDir = Join-Path $Root "gamedata\dev_hdd0\home\00000001\savedata"
$Slot = Join-Path $SaveDir "BCES01741-SYSDATA"
if (-not (Test-Path $Exe)) { Write-Host "missing $Exe (build the ASCENSION_PGO=GEN tree first)"; exit 1 }
New-Item -ItemType Directory -Force $Out | Out-Null
# The DLLs the game loads (build.ps1 copies them next to build\win's exe).
Get-ChildItem (Join-Path $Root "build\win") -Filter *.dll | Copy-Item -Destination (Split-Path $Exe) -Force
Get-Process AscensionRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
if (Test-Path $Slot) {
    $dest = Join-Path $Work ("save-moved\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Force $dest | Out-Null; Move-Item $Slot $dest
}
Copy-Item -Recurse (Join-Path $Work "save-backup\BCES01741-SYSDATA") $SaveDir
$press = @(14, 24, 27, 30, 33) | ForEach-Object { $t = [int]([Math]::Round($_ * $Slow)); "${t}:0x4000,$($t + 1):0x0000" }
$env:PAD_SCRIPT = $press -join ","
$env:PAD_PLAY = [string][int](100 * $Slow)
$env:PS3_MODS = Join-Path $env:APPDATA "ascension-launcher\generated"
$env:LLVM_PROFILE_FILE = Join-Path $Out "game-%p.profraw"
$t0 = Get-Date
Start-Process cmd.exe -ArgumentList "/c `"`"$Exe`" `"$Elf`" > `"$Log`" 2>&1`"" -WorkingDirectory $Root -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
$game = Get-Process AscensionRecomp -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $t0.AddSeconds(-1) } | Select-Object -First 1
if (-not $game) { Write-Host "the game did not start"; exit 1 }
Write-Host "training run: pid $($game.Id), $Seconds s (presses: $($env:PAD_SCRIPT))"
Wait-Process -Id $game.Id -Timeout ($Seconds - 3) -ErrorAction SilentlyContinue
$game.Refresh()
if (-not $game.HasExited) {
    $game.CloseMainWindow() | Out-Null          # the game writes its profile, then ends
    if (-not $game.WaitForExit(60000)) { Write-Host "the game did not close: killing it (no profile)"; $game.Kill() }
}
Get-ChildItem $Out -Filter *.profraw | Select-Object Name, Length
Select-String -Path $Log -Pattern "PGO profile|\[perf\]" | Select-Object -Last 4 | ForEach-Object { $_.Line }
