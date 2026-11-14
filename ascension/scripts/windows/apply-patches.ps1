# Apply RPCS3 game patches to the recompiled game and rebuild what they touch.
#
#   .\scripts\windows\apply-patches.ps1 -List
#   .\scripts\windows\apply-patches.ps1 -Enable "Disable SPU MLAA - God of War: Ascension (01)"
#   .\scripts\windows\apply-patches.ps1 -Enable @()          # back to the unpatched game
#   .\scripts\windows\apply-patches.ps1 -FromLauncher        # the patches ticked in the launcher
#   .\scripts\windows\apply-patches.ps1 -Runtime [-FromLauncher | -Enable ...]
#
# -Runtime (2026-11-01): every patch becomes a switch read when the game starts
# (PS3_PATCHES, host\ascension_patches.cpp; the launcher passes its ticks), so
# changing patches needs no translation or build. The unpatched executable is
# translated once with the instructions the patches change lifted both ways
# (ppu_lifter.py --patch-sites), the SPU programs they change are kept in both
# versions (meta\spu\spu_xorig_*.elf), and tools\rpcs3_patch.py runtime writes
# the table of patch bytes (host\ascension_patch_table.inc). The ticked or
# -Enable patches are only the build's defaults (PS3_PATCHES unset). After
# that, -FromLauncher alone does nothing: the tree is marked as runtime.
#
# The pristine decrypted executable is work\pristine\EBOOT.ELF; the patched one
# is written to work\disc\PS3_GAME\USRDIR\EBOOT.ELF, which the game loads. Code
# patches only take effect once the code is translated again, so:
#   - PPU code bytes changed  -> the PPU lift is redone (about a minute)
#   - an SPU program changed  -> the SPU programs are lifted again (seconds)
#   - data only               -> nothing to translate
# then build.ps1 runs. Patch files: work\patches\patch.yml (RPCS3's, refreshed
# with -Update) and patches-rpcs3\ascension-1.00.yml (patches ported to 1.00).
param(
    [string[]]$Enable = $null,
    [switch]$List,
    [switch]$FromLauncher,
    [switch]$Update,
    [switch]$NoBuild,
    [switch]$Runtime
)
. (Join-Path $PSScriptRoot "env.ps1")
$Work     = $Cfg.WORK_DIR
$Tools    = Join-Path $Cfg.PS3KIT_DIR "tools"
$Pristine = Join-Path $Work "pristine\EBOOT.ELF"
$Target   = Join-Path $Work "disc\PS3_GAME\USRDIR\EBOOT.ELF"
$PatchDir = Join-Path $Work "patches"
$Tool     = Join-Path $Root "tools\rpcs3_patch.py"
$Logs     = Join-Path $Root "build\logs"
New-Item -ItemType Directory -Force $Logs, $PatchDir | Out-Null
if (-not (Test-Path $Pristine)) { Write-Host "missing $Pristine (the decrypted, unpatched EBOOT.ELF)"; exit 1 }

# RPCS3's own patch file is only downloaded when asked (-Update); without it only
# patches-rpcs3\ascension-1.00.yml is used.
if ($Update) {
    Write-Host "downloading RPCS3's patch file"
    $resp = Invoke-RestMethod "https://rpcs3.net/compatibility?patch&api=v1&v=1.2"
    if ($resp.return_code -ne 0) { Write-Host "patch download failed (return code $($resp.return_code))"; exit 1 }
    [IO.File]::WriteAllText((Join-Path $PatchDir "patch.yml"), $resp.patch, (New-Object Text.UTF8Encoding $false))
}
$PatchFiles = @((Join-Path $Root "patches-rpcs3\ascension-1.00.yml"))
if (Test-Path (Join-Path $PatchDir "patch.yml")) { $PatchFiles = @((Join-Path $PatchDir "patch.yml")) + $PatchFiles }

if ($List) { python $Tool info --elf $Pristine --patches $PatchFiles; exit $LASTEXITCODE }

if ($FromLauncher) {
    $settings = Join-Path $env:APPDATA "ascension-launcher\settings.json"
    $Enable = @()
    if (Test-Path $settings) {
        $s = Get-Content $settings -Raw | ConvertFrom-Json
        if ($s.rpcs3_patches) { $Enable = @($s.rpcs3_patches) }
    }
}
if ($null -eq $Enable -and $Runtime) { $Enable = @() }
if ($null -eq $Enable) { Write-Host "pass -Enable <names>, -FromLauncher or -List"; exit 1 }

function Get-TextHash([string]$elf) {
    python -I -c "import hashlib,struct,sys;e=open(sys.argv[1],'rb').read();po,=struct.unpack_from('>Q',e,0x20);pe,pn=struct.unpack_from('>HH',e,0x36);h=hashlib.sha1()
for i in range(pn):
  t,f,o,v,p,fs,ms,a=struct.unpack_from('>IIQQQQQQ',e,po+i*pe)
  if t==1 and f&1: h.update(e[o:o+min(fs,0x62C4CC-v)])
print(h.hexdigest())" $elf
}
$Meta   = Join-Path $Root "meta"
$Recomp = Join-Path $Root "src\recomp"
$Spu    = Join-Path $Root "src\spu"
$stamp  = Join-Path $Recomp ".lifted-from"
$lifted = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }

# The post-lift hooks (both modes): the VRAM manager's copy/move guard
# (PPU_VRAM_GUARD, host/ascension_vram_guard.cpp), the VRAM plan hooks
# (PPU_VRAM_PLAN), the Z pre-pass hook (PPU_ZPREPASS_SKIP) and the texture
# streaming budget hook (TEX_STREAM_BUDGET) live in the
# lifted code: a launcher patch change on 2026-10-31 relifted without the last
# two, and the next build would have quietly lost both.
function Add-LiftHooks {
    python (Join-Path $Root "tools\vram_guard_patch.py") $Recomp
    if ($LASTEXITCODE -ne 0) { Write-Host "vram_guard_patch failed"; exit 1 }
    python (Join-Path $Root "tools\vram_plan_patch.py") apply $Recomp
    if ($LASTEXITCODE -ne 0) { Write-Host "vram_plan_patch failed"; exit 1 }
    python (Join-Path $Root "tools\zprepass_patch.py") $Recomp
    if ($LASTEXITCODE -ne 0) { Write-Host "zprepass_patch failed"; exit 1 }
    python (Join-Path $Root "tools\texstream_patch.py") $Recomp
    if ($LASTEXITCODE -ne 0) { Write-Host "texstream_patch failed"; exit 1 }
    python (Join-Path $Root "tools\gamelog_patch.py") $Recomp
    if ($LASTEXITCODE -ne 0) { Write-Host "gamelog_patch failed"; exit 1 }
}

if ($Runtime) {
    $Sites = Join-Path $Meta "patch_sites.json"
    $Table = Join-Path $Root "host\ascension_patch_table.inc"
    $AllOn = Join-Path $PatchDir "all-on.ELF"
    $argsR = @("runtime", "--elf", $Pristine, "--table", $Table, "--sites", $Sites, "--out", $AllOn, "--patches") + $PatchFiles
    if ($Enable.Count) { $argsR += @("--enable") + $Enable }
    python $Tool @argsR
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $text = "runtime:" + (Get-TextHash $Pristine) + ":" + (Get-FileHash $Sites -Algorithm SHA1).Hash
    if ($text -ne $lifted) {
        Write-Host "translating the unpatched PPU code with the patch sites (log: build\logs\relift-ppu.log)"
        python (Join-Path $Tools "ppu_lifter.py") $Pristine --functions (Join-Path $Meta "functions.json") `
            --hle-stubs (Join-Path $Meta "EBOOT.imports.json") --toc 0x81EF60 --code-end 0x62C4CC `
            --chunk-lines 150000 -j 16 --patch-sites $Sites -o $Recomp *> (Join-Path $Logs "relift-ppu.log")
        if ($LASTEXITCODE -ne 0) { Write-Host "PPU lift failed"; exit 1 }
        Add-LiftHooks
        Set-Content -NoNewline -Path $stamp -Value $text
    } else { Write-Host "PPU code already translated with the patch sites" }
    # SPU programs: as the patches leave them, plus the original of each one a
    # patch changes (spu_xorig_*, sorted after the others so the image numbers
    # the runtime knows stay put); both are matched by their bytes at load.
    $tmpOn = Join-Path $env:TEMP "ascension-spu-allon"; $tmpOrig = Join-Path $env:TEMP "ascension-spu-orig"
    foreach ($d in $tmpOn, $tmpOrig) { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $d; New-Item -ItemType Directory -Force $d | Out-Null }
    python (Join-Path $Tools "extract_spu_images.py") $AllOn -o $tmpOn | Out-Null
    python (Join-Path $Tools "extract_spu_images.py") $Pristine -o $tmpOrig | Out-Null
    $want = @{}
    foreach ($f in Get-ChildItem $tmpOn -Filter *.elf) {
        $want[$f.Name] = $f.FullName
        $o = Join-Path $tmpOrig $f.Name
        if ((Test-Path $o) -and (Get-FileHash $o).Hash -ne (Get-FileHash $f.FullName).Hash) { $want["spu_xorig_" + $f.Name.Substring(4)] = $o }
    }
    $changed = @()
    foreach ($k in $want.Keys) {
        $old = Join-Path $Meta "spu\$k"
        if (-not (Test-Path $old) -or (Get-FileHash $old).Hash -ne (Get-FileHash $want[$k]).Hash) {
            Copy-Item $want[$k] $old -Force; $changed += [IO.Path]::GetFileNameWithoutExtension($k)
        }
    }
    if ($changed.Count) {
        # Only the changed images are translated again (register-local form, as
        # relift-spu.ps1): --relift would drop the native/record wrappers
        # spu_wrap.py put into the others.
        foreach ($c in $changed) { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Spu $c) }
        Write-Host "SPU programs changed ($($changed -join ', ')): translating them (log: build\logs\relift-spu.log)"
        $env:SPU_LIFT_XREGS = "1"
        python (Join-Path $Tools "build_spu_workloads.py") --images (Join-Path $Meta "spu") --lifted $Spu `
            --out (Join-Path $Spu "spu_workloads.c") --register-fn ascension_spu_register_all --constructor `
            --title ascension --extra-funcs "spu_wws_pm_at_0077CA80=0xA2C,0xA68,0x3368,0x3378" `
            *> (Join-Path $Logs "relift-spu.log")
        $rc = $LASTEXITCODE
        Remove-Item Env:SPU_LIFT_XREGS -ErrorAction SilentlyContinue
        if ($rc -ne 0) { Write-Host "SPU lift failed"; exit 1 }
    } else { Write-Host "SPU programs unchanged" }
    if (-not $NoBuild) { & (Join-Path $PSScriptRoot "build.ps1") -Name patches; exit $LASTEXITCODE }
    exit 0
}
if ($lifted.StartsWith("runtime:")) {
    Write-Host "This build switches the patches when the game starts (PS3_PATCHES): nothing to translate or build."
    exit 0
}

# 1. The patched executable.
$summary = Join-Path $Root "build\patches-applied.json"
$args1 = @("apply", "--elf", $Pristine, "--out", $Target, "--summary", $summary, "--patches") + $PatchFiles
if ($Enable.Count) { $args1 += @("--enable") + $Enable }
python $Tool @args1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 2. What has to be translated again: compare with what the current lift came
# from (Get-TextHash: the lifted range only; the executable segment also
# carries the embedded SPU programs and read-only data after the code).
$text  = Get-TextHash $Target
if ($text -ne $lifted) {
    Write-Host "PPU code changed: translating it again (log: build\logs\relift-ppu.log)"
    python (Join-Path $Tools "ppu_lifter.py") $Target --functions (Join-Path $Meta "functions.json") `
        --hle-stubs (Join-Path $Meta "EBOOT.imports.json") --toc 0x81EF60 --code-end 0x62C4CC `
        --chunk-lines 150000 -j 16 -o $Recomp *> (Join-Path $Logs "relift-ppu.log")
    if ($LASTEXITCODE -ne 0) { Write-Host "PPU lift failed"; exit 1 }
    Add-LiftHooks
    Set-Content -NoNewline -Path $stamp -Value $text
} else { Write-Host "PPU code unchanged" }

# SPU programs: extract them from the patched executable and compare.
$tmp = Join-Path $env:TEMP "ascension-spu-extract"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tmp
New-Item -ItemType Directory -Force $tmp | Out-Null
python (Join-Path $Tools "extract_spu_images.py") $Target -o $tmp | Out-Null
python (Join-Path $Root "tools\extract_spu_pm.py") $Target (Join-Path $tmp "spu_wws_pm_at_0077CA80.elf") `
    --address 0x77CA80 --size 12288 --fnv 0xC8D774963A96DCF2 | Out-Null
$changed = $false
foreach ($f in Get-ChildItem $tmp -Filter *.elf) {
    $old = Join-Path $Meta "spu\$($f.Name)"
    if (-not (Test-Path $old) -or (Get-FileHash $old).Hash -ne (Get-FileHash $f.FullName).Hash) { $changed = $true }
}
if ($changed) {
    Write-Host "SPU programs changed: translating them again (log: build\logs\relift-spu.log)"
    Copy-Item (Join-Path $tmp "*.elf") (Join-Path $Meta "spu") -Force
    python (Join-Path $Tools "build_spu_workloads.py") --images (Join-Path $Meta "spu") --lifted $Spu `
        --out (Join-Path $Spu "spu_workloads.c") --register-fn ascension_spu_register_all --constructor `
        --title ascension --relift --extra-funcs "spu_wws_pm_at_0077CA80=0xA2C,0xA68,0x3368,0x3378" `
        *> (Join-Path $Logs "relift-spu.log")
    if ($LASTEXITCODE -ne 0) { Write-Host "SPU lift failed"; exit 1 }
} else { Write-Host "SPU programs unchanged" }

# 3. Build.
if (-not $NoBuild) { & (Join-Path $PSScriptRoot "build.ps1") -Name patches; exit $LASTEXITCODE }
