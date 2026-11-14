# Run the game for a while, with the window visible, and summarise the log.
#
#   .\scripts\windows\run.ps1                          # 120 s, no save (first-use menus)
#   .\scripts\windows\run.ps1 -Save backup -Seconds 90 # work\save-backup in place: straight to the cinematic
#   .\scripts\windows\run.ps1 -Name cin -Env @{ VDEC_TIMELOG = "1"; PS3_HLE_HIST = "5" }
#   .\scripts\windows\run.ps1 -Profile -ProfileAt 45   # sample the busiest threads at 45 s
#   .\scripts\windows\run.ps1 -Seconds 0 -Save keep    # play: runs until you close the window
#   .\scripts\windows\run.ps1 -Menu -Seconds 60        # straight to the main menu (~15 s)
#   .\scripts\windows\run.ps1 -Level -Rpcs3 -Seconds 150  # into the first level (~100 s), on RPCS3's RSX
#
# -Save fresh  : move the current save aside (work\save-moved\<time>) so the
#                first-use config menus show up (the default)
# -Save backup : as fresh, then copy work\save-backup in (skips those menus)
# -Save keep   : leave the save alone
# -CaptureAt N : presses F12 in the game window at N s: one frame's draws,
#                shaders and surfaces land in captures\cap_<frame>\.
# -Menu        : save-backup, the launcher's composed mods (skip intro) and
#                one X press (14 s) that skips the opening cinematic; the menu is up in
#                ~15 s. More presses enter the second menu (Single Player).
# -Level       : as -Menu, with the presses that start the game: the first level
#                (Kratos chained) is up in ~100 s.
# -Rpcs3       : PS3_RSX=rpcs3 (RPCS3's RSX draws; the build needs RPCS3_RSX=1).
# The log is build\logs\<Name>.log; tools\log_summary.py prints the summary,
# and with -Profile the resolved profile is build\logs\<Name>-prof.txt.
param(
    [int]$Seconds = 120,
    [ValidateSet("fresh", "backup", "keep")][string]$Save = "fresh",
    [string]$Name = "run",
    $Env = @{},                         # hashtable, or "NAME=value;NAME2=value2"
    [switch]$Profile,
    [int]$ProfileAt = 45,
    [int]$ProfileThreads = 4,
    [int]$ProfileHeavy = 0,             # with -Profile: sample at the first heavy window after -ProfileAt (draws a frame)
    [ValidateSet("prof", "profip")][string]$ProfileVerb = "prof",   # profip: exclusive time per instruction
    [switch]$Menu,
    [switch]$Level,
    [switch]$Rpcs3,
    [int]$CaptureAt = 0,                # press F12 (frame capture into captures\) at this second
    [string]$Exe = ""                   # another build in build\win (e.g. AscensionRecomp.head1.exe); default AscensionRecomp.exe
)

. (Join-Path $PSScriptRoot "env.ps1")
$Work    = $Cfg.WORK_DIR
$Exe     = if ($Exe) { if ([IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $Root "build\win\$Exe" } } else { Join-Path $Root "build\win\AscensionRecomp.exe" }
if (-not (Test-Path $Exe)) { Write-Host "missing $Exe"; exit 1 }
# The process is named after the file (AscensionRecomp.head1.exe -> "AscensionRecomp.head1").
$ProcName = [IO.Path]::GetFileNameWithoutExtension($Exe)
$Elf     = Join-Path $Work "disc\PS3_GAME\USRDIR\EBOOT.ELF"
$Logs    = Join-Path $Root "build\logs"
$SaveDir = Join-Path $Root "gamedata\dev_hdd0\home\00000001\savedata"
$Slot    = Join-Path $SaveDir "BCES01741-SYSDATA"
New-Item -ItemType Directory -Force $Logs, $SaveDir | Out-Null
$Log = Join-Path $Logs "$Name.log"

if ($Level) { $Menu = $true }
if ($Menu) {
    if (-not $PSBoundParameters.ContainsKey("Save")) { $Save = "backup" }
    if ($Env -is [string]) { $Env = $Env + ";" } else { $Env = $Env.Clone() }
    $pad = if ($Level) { "14:0x4000,15:0x0000,24:0x4000,25:0x0000,27:0x4000,28:0x0000,30:0x4000,31:0x0000,33:0x4000,34:0x0000" }
           else { "14:0x4000,15:0x0000" }
    $menuEnv = @{ PAD_SCRIPT = $pad }
    $gen = Join-Path $env:APPDATA "ascension-launcher\generated"
    if (Test-Path $gen) { $menuEnv["PS3_MODS"] = $gen } else { Write-Host "no launcher mods at $($gen): the intro screens will play" }
    if ($Env -is [string]) { foreach ($k in $menuEnv.Keys) { $Env += "$k=$($menuEnv[$k]);" } }
    else { foreach ($k in $menuEnv.Keys) { if (-not $Env.ContainsKey($k)) { $Env[$k] = $menuEnv[$k] } } }
}
$movedSave = $null   # the save that was in the slot before this run, put back afterwards
if ($Save -ne "keep" -and (Test-Path $Slot)) {
    $dest = Join-Path $Work ("save-moved\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Force $dest | Out-Null
    Move-Item $Slot $dest
    $movedSave = Join-Path $dest (Split-Path $Slot -Leaf)
    Write-Host "save moved to $dest"
}
if ($Save -eq "backup") {
    Copy-Item -Recurse (Join-Path $Work "save-backup\BCES01741-SYSDATA") $SaveDir
    Write-Host "save-backup copied in"
}

if ($Env -is [string]) {
    $h = @{}
    foreach ($kv in ($Env -split ";")) { if ($kv -match "^\s*([^=]+)=(.*)$") { $h[$Matches[1].Trim()] = $Matches[2] } }
    $Env = $h
} elseif (-not $Menu) { $Env = $Env.Clone() }
# -Rpcs3 holds for every run, not only -Menu/-Level ones.
if ($Rpcs3 -and -not $Env.ContainsKey("PS3_RSX")) { $Env["PS3_RSX"] = "rpcs3" }
# Timed test runs (soak.ps1, benchmarks) are silent: AUDIO_MUTE=1 keeps the
# audio clock and pacing but sends silence. Play mode (-Seconds 0) keeps its
# sound; -Env @{ AUDIO_MUTE = "0" } turns it back on for a test.
if ($Seconds -gt 0 -and -not $Env.ContainsKey("AUDIO_MUTE")) { $Env["AUDIO_MUTE"] = "1" }
# The game inherits this process's environment; restore it afterwards.
$saved = @{}
foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, [string]$Env[$k]) }
$dbg = Join-Path $Logs "$Name-dbg.txt"
if ($Profile) {
    Remove-Item -ErrorAction SilentlyContinue $dbg, "$dbg.out"
    $saved["PS3_DEBUG"] = $env:PS3_DEBUG; $env:PS3_DEBUG = $dbg
}

# One game at a time: close any copy that is still open before starting.
# Only the playable copy (build\win): private builds (build\agent*, replays)
# are other people's runs and stay.
$old = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -eq [IO.Path]::GetFullPath($Exe)) }
if ($old) {
    Write-Host "closing the game that is already running (pid $($old.Id -join ', '))"
    $old | Stop-Process -Force
    $old | ForEach-Object { $_.WaitForExit(5000) | Out-Null }
}

$t0 = Get-Date
$p = Start-Process cmd.exe -ArgumentList "/c `"`"$Exe`" `"$Elf`" > `"$Log`" 2>&1`"" -WorkingDirectory $Root -PassThru -WindowStyle Hidden
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
Write-Host $(if ($Seconds -le 0) { "running until closed (log: $Log)" } else { "running for $Seconds s (log: $Log)" })

# The game started by this script: the newest one launched after $t0. Look
# for up to 20 s: a slow start once left $game empty, the timed wait and the
# close were skipped and the game ran on unattended after the script ended.
$game = $null
for ($try = 0; $try -lt 40 -and -not $game; $try++) {
    Start-Sleep -Milliseconds 500
    $game = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $t0.AddSeconds(-1) -and $_.Path -and ([IO.Path]::GetFullPath($_.Path) -eq [IO.Path]::GetFullPath($Exe)) } |
        Sort-Object StartTime -Descending | Select-Object -First 1
}
if (-not $game) { Write-Host "the game process was not found after 20 s" }
if ($Profile -and $game) {
    $wait = $ProfileAt - ((Get-Date) - $t0).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Seconds $wait }
    # -ProfileHeavy N: from -ProfileAt on, wait for a 5 s window of N or more
    # draws a frame ([draws] per 5s over the [perf] fps) before sampling, so
    # a scene that comes at a varying time is the one profiled.
    if ($ProfileHeavy -gt 0) {
        $limit = $t0.AddSeconds($(if ($Seconds -gt 0) { $Seconds - 15 } else { 600 }))
        $seen = 0
        while ((Get-Date) -lt $limit) {
            Start-Sleep -Seconds 2
            # Only the [perf]/[draws] lines: logging options can add thousands
            # of other lines between them.
            $lines = Get-Content $Log -Tail 30000 -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\[(perf|draws)\]' }
            $fps = @(); $dpf = 0
            foreach ($l in $lines) {
                if ($l -match '^\[perf\] fps=([\d.]+)') { $fps += [double]$Matches[1] }
                elseif ($l -match '^\[draws\] per 5s: (\d+) draws') {
                    $f = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }
                    $dpf = if ($f -gt 1) { [int]([double]$Matches[1] / (5 * $f)) } else { 0 }
                    $fps = @()
                }
            }
            if ($dpf -ge $ProfileHeavy) { $seen++ } else { $seen = 0 }
            if ($seen -ge 2) { Write-Host "heavy ($dpf draws a frame) at $([int]((Get-Date) - $t0).TotalSeconds) s: profiling"; break }
        }
        if ($seen -lt 2) { Write-Host "no heavy window before the end: not profiled" }
    }
    if ($ProfileHeavy -le 0 -or $seen -ge 2) {
    python -I (Join-Path $Root "tools\profile_threads.py") --pid $game.Id --dbg $dbg --threads $ProfileThreads `
        --verb $ProfileVerb --map $(if (Test-Path ([IO.Path]::ChangeExtension($Exe, ".map"))) { [IO.Path]::ChangeExtension($Exe, ".map") } else { Join-Path $Root "build\win\AscensionRecomp.map" }) --out (Join-Path $Logs "$Name-prof.txt")
    }
}
if ($CaptureAt -gt 0 -and $game) {
    $wait = $CaptureAt - ((Get-Date) - $t0).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Seconds $wait }
    Add-Type -Namespace RunPs1 -Name Keys -MemberDefinition @"
[DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, System.UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
"@
    $game.Refresh()
    [RunPs1.Keys]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
    [RunPs1.Keys]::keybd_event(0x7B, 0, 0, [System.UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    [RunPs1.Keys]::keybd_event(0x7B, 0, 2, [System.UIntPtr]::Zero)
    Write-Host "F12 sent at $([int]((Get-Date) - $t0).TotalSeconds) s"
}
if ($Seconds -le 0) {
    # -Seconds 0: play; wait until the game is closed, never kill it.
    if ($game) { Write-Host "playing: close the game window when done"; Wait-Process -Id $game.Id }
} else {
    $left = $Seconds - ((Get-Date) - $t0).TotalSeconds
    if ($left -gt 0 -and $game) { Wait-Process -Id $game.Id -Timeout $left -ErrorAction SilentlyContinue }
    # Only the copy this script started: never a game someone is playing.
    # Close the window first, as a player would, so the game saves what it
    # keeps between runs (the compiled GPU pipelines); kill it if it hangs.
    if ($game -and -not $game.HasExited) {
        $game.CloseMainWindow() | Out-Null
        if (-not $game.WaitForExit(10000)) { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }
    }
}
Start-Sleep -Milliseconds 500

# A timed run used its own save: put the slot back as it was, so the player's
# progress is where the game looks for it (it used to stay in save-moved\).
if ($Seconds -gt 0 -and $Save -ne "keep" -and (-not $game -or $game.HasExited)) {
    if (Test-Path $Slot) { Remove-Item -Recurse -Force $Slot }
    if ($movedSave -and (Test-Path $movedSave)) {
        Move-Item $movedSave $SaveDir
        Write-Host "save put back from $(Split-Path $movedSave -Parent)"
    }
}

python -I (Join-Path $Root "tools\log_summary.py") $Log
