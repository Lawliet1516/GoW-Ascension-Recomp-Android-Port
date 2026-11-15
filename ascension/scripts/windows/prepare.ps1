# Make the game-specific sources from YOUR OWN decrypted EBOOT.ELF. Nothing
# made here may be shared: it is translated game code (it stays out of git,
# see .gitignore).
#
#   .\scripts\windows\prepare.ps1            # everything, then run build.ps1
#   .\scripts\windows\prepare.ps1 -Force     # redo the steps that are already done
#
# Input: <WORK_DIR>\pristine\EBOOT.ELF (the decrypted, unpatched disc EBOOT of
# God of War: Ascension BCES01741 1.00; WORK_DIR comes from .env).
# Output (all generated, never committed):
#   <WORK_DIR>\jobmods\jobmod_*.bin      the 33 SPU job modules carved from the EBOOT
#   <WORK_DIR>\patches\all-on.ELF        the EBOOT with every game patch applied (only read here)
#   meta\patch_sites.json                the instructions the game patches change
#   meta\spu\*.elf                       the SPU programs embedded in the EBOOT
#   host\ascension_patch_table.inc       the patch switch table (host\ascension_patches.cpp)
#   src\recomp\                          the PPU code, translated to C++ (12 files, ~1.3 GB of text)
#   src\spu\                             the SPU programs, translated to C
# Python 3.10+ with the packages in ..\ps3kit\requirements.txt. Takes about
# 5-15 minutes; logs in build\logs\prepare-*.log.
param([switch]$Force)
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "env.ps1")
$Work     = $Cfg.WORK_DIR
$Kit      = Join-Path $Cfg.PS3KIT_DIR "tools"
$Tools    = Join-Path $Root "tools"
$Meta     = Join-Path $Root "meta"
$Recomp   = Join-Path $Root "src\recomp"
$Spu      = Join-Path $Root "src\spu"
$Logs     = Join-Path $Root "build\logs"
$Pristine = Join-Path $Work "pristine\EBOOT.ELF"
$PatchDir = Join-Path $Work "patches"
$AllOn    = Join-Path $PatchDir "all-on.ELF"
$Sites    = Join-Path $Meta "patch_sites.json"
$Table    = Join-Path $Root "host\ascension_patch_table.inc"
New-Item -ItemType Directory -Force $Logs, $PatchDir, (Join-Path $Meta "spu") | Out-Null
$py = if ($env:PYTHON) { $env:PYTHON } else { "python" }

function Step([string]$Name, [string]$Log, [scriptblock]$Body) {
    Write-Host "== $Name (log: $Log)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Body *> $Log
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $Name (exit $LASTEXITCODE). Last lines of $Log`:"; Get-Content $Log -Tail 15; exit 1 }
    Write-Host ("   done in {0:mm\:ss}" -f $sw.Elapsed)
}

if (-not (Test-Path $Pristine)) {
    Write-Host "missing $Pristine"
    Write-Host "Decrypt your own disc's PS3_GAME\USRDIR\EBOOT.BIN to EBOOT.ELF (RPCS3: Utilities > Decrypt PS3 Binaries,"
    Write-Host "or tools\decrypt_self.py with your own key file) and put it there."
    exit 1
}
$md5 = (Get-FileHash $Pristine -Algorithm MD5).Hash
Write-Host "EBOOT.ELF md5 $md5"

# 1. The SPU job modules (carved by their layout; the names are the SHA-1 of the bytes).
$jm = Join-Path $Work "jobmods"
if ($Force -or -not (Test-Path (Join-Path $jm "jobmod_7868e9b847d1_e6D98.bin"))) {
    Step "carve the SPU job modules" (Join-Path $Logs "prepare-jobmods.log") {
        & $py -I (Join-Path $Tools "jobmods_carve.py") $Pristine (Join-Path $Tools "jobmods_extra_funcs.json") $jm trailer+0 --write
    }
    if (-not (Test-Path (Join-Path $jm "jobmod_7868e9b847d1_e6D98.bin"))) {
        Write-Host "the job modules do not match the known 1.00 disc EBOOT (is it BCES01741 1.00, decrypted?)"; exit 1
    }
}

# 2. The game patches as run-time switches (tools\rpcs3_patch.py runtime): the
#    table of patch bytes, the patch sites the PPU lift translates both ways and
#    the all-patches-on EBOOT the SPU programs are compared with. Patch files:
#    patches-rpcs3\ascension-1.00.yml (in this repository), plus RPCS3's own
#    patch.yml if you put a copy at <WORK_DIR>\patches\patch.yml (optional;
#    nothing is downloaded).
$PatchFiles = @((Join-Path $Root "patches-rpcs3\ascension-1.00.yml"))
$rp = Join-Path $PatchDir "patch.yml"
if (Test-Path $rp) { $PatchFiles = @($rp) + $PatchFiles }
Step "patch table" (Join-Path $Logs "prepare-patches.log") {
    & $py -I (Join-Path $Tools "rpcs3_patch.py") runtime --elf $Pristine --table $Table --sites $Sites --out $AllOn --patches @PatchFiles
}

# 3. The PPU code -> src\recomp, then the post-lift hooks (scripts\windows\apply-patches.ps1 does the same).
$stamp = Join-Path $Recomp ".lifted-from"
if ($Force -or -not (Test-Path (Join-Path $Recomp "ppu_recomp.h"))) {
    Step "translate the PPU code" (Join-Path $Logs "prepare-ppu.log") {
        & $py (Join-Path $Kit "ppu_lifter.py") $Pristine --functions (Join-Path $Meta "functions.json") `
            --hle-stubs (Join-Path $Meta "EBOOT.imports.json") --toc 0x81EF60 --code-end 0x62C4CC `
            --chunk-lines 150000 -j 16 --patch-sites $Sites -o $Recomp
    }
    Step "post-lift hooks" (Join-Path $Logs "prepare-hooks.log") {
        & $py (Join-Path $Tools "vram_guard_patch.py") $Recomp; if ($LASTEXITCODE) { return }
        & $py (Join-Path $Tools "vram_plan_patch.py") apply $Recomp; if ($LASTEXITCODE) { return }
        & $py (Join-Path $Tools "zprepass_patch.py") $Recomp; if ($LASTEXITCODE) { return }
        & $py (Join-Path $Tools "texstream_patch.py") $Recomp; if ($LASTEXITCODE) { return }
        & $py (Join-Path $Tools "gamelog_patch.py") $Recomp; if ($LASTEXITCODE) { return }
        & $py -I (Join-Path $Tools "mksrc.py") $Recomp   # online: SSL off / TSS plain (switched) + 2 NULL guards
    }
    Set-Content -NoNewline -Path $stamp -Value ("runtime:" + $md5 + ":" + (Get-FileHash $Sites -Algorithm SHA1).Hash)
}

# 4. The SPU programs embedded in the EBOOT -> meta\spu (as the patches leave
#    them, plus the original of each one a patch changes: spu_xorig_*), and the
#    Wws policy module, which the EBOOT carries as raw SPU code.
$tmpOn = Join-Path $env:TEMP "ascension-prep-allon"; $tmpOrig = Join-Path $env:TEMP "ascension-prep-orig"
foreach ($d in $tmpOn, $tmpOrig) { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $d; New-Item -ItemType Directory -Force $d | Out-Null }
Step "extract the SPU programs" (Join-Path $Logs "prepare-spu-extract.log") {
    & $py (Join-Path $Kit "extract_spu_images.py") $AllOn -o $tmpOn; if ($LASTEXITCODE) { return }
    & $py (Join-Path $Kit "extract_spu_images.py") $Pristine -o $tmpOrig; if ($LASTEXITCODE) { return }
    & $py (Join-Path $Tools "extract_spu_pm.py") $Pristine (Join-Path $Meta "spu\spu_wws_pm_at_0077CA80.elf") `
        --address 0x77CA80 --size 12288 --fnv 0xC8D774963A96DCF2
}
foreach ($f in Get-ChildItem $tmpOn -Filter *.elf) {
    Copy-Item $f.FullName (Join-Path $Meta "spu\$($f.Name)") -Force
    $o = Join-Path $tmpOrig $f.Name
    if ((Test-Path $o) -and (Get-FileHash $o).Hash -ne (Get-FileHash $f.FullName).Hash) {
        Copy-Item $o (Join-Path $Meta ("spu\spu_xorig_" + $f.Name.Substring(4))) -Force
    }
}
Write-Host ("   {0} SPU programs in meta\spu" -f (Get-ChildItem (Join-Path $Meta "spu") -Filter *.elf).Count)

# 5. The SPU programs -> src\spu (register-local form, as relift-spu.ps1).
if ($Force -or -not (Test-Path (Join-Path $Spu "spu_workloads.c"))) {
    New-Item -ItemType Directory -Force $Spu | Out-Null
    $env:SPU_LIFT_XREGS = "1"
    try {
        $env:LIFT_JOBMODS_MERGE = "1"
        Step "translate the SPU job modules" (Join-Path $Logs "prepare-spu-jobmods.log") {
            & $py (Join-Path $Kit "lift_jobmods.py") --jobmods $jm --out $Spu --register (Join-Path $Spu "jobmods_register.c") `
                --symbol-prefix ascension --extra-funcs-json (Join-Path $Tools "jobmods_extra_funcs.json")
        }
        Remove-Item Env:LIFT_JOBMODS_MERGE -ErrorAction SilentlyContinue
        Step "translate the SPU code fragments" (Join-Path $Logs "prepare-spu-fragments.log") {
            & $py (Join-Path $Kit "lift_spu_pic.py") --elf $Pristine --fragments (Join-Path $Tools "job_fragments.json") `
                --out $Spu --register (Join-Path $Spu "pic_register.c") --symbol-prefix ascension
        }
        Step "translate the SPU programs" (Join-Path $Logs "prepare-spu-images.log") {
            & $py (Join-Path $Kit "build_spu_workloads.py") --images (Join-Path $Meta "spu") --lifted $Spu `
                --out (Join-Path $Spu "spu_workloads.c") --register-fn ascension_spu_register_all --constructor `
                --title ascension --relift --extra-funcs "spu_wws_pm_at_0077CA80=0xA2C,0xA68,0x3368,0x3378"
        }
    } finally {
        Remove-Item Env:SPU_LIFT_XREGS, Env:LIFT_JOBMODS_MERGE -ErrorAction SilentlyContinue
    }
}
Write-Host "prepared. Next: .\scripts\windows\build.ps1"
