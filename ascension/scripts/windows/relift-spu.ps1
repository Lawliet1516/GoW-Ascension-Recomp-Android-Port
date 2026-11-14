# Re-lift EVERY SPU image (the 33 job modules, the 42 code fragments, the 5
# workload images) into a separate folder, in the register-local form
# (spu_lifter.py --xregs) unless -Plain, and re-apply the record/replay/native
# wrappers that src\spu has now (read from its spu_recomp.c files, so whatever
# another agent wrapped meanwhile is kept).
#
#   .\scripts\windows\relift-spu.ps1                        # -> build\spu-xregs
#   .\scripts\windows\relift-spu.ps1 -Out build\spu-plain -Plain
#   .\scripts\windows\relift-spu.ps1 -Install               # then copy it over src\spu
#                                                           # (src\spu saved to build\spu-backup-<time>)
# Build against the folder without installing it:
#   cmake -B build\agentN -DSPU_DIR="<full path of the folder>"
# Faster helpers too: -DASCENSION_SPU_CODEGEN2=ON (or SPU_CODEGEN2=1 in .env).
param([string]$Out = "build\spu-xregs", [switch]$Plain, [switch]$Install)
. (Join-Path $PSScriptRoot "env.ps1")
$Work  = $Cfg.WORK_DIR
$Tools = Join-Path $Cfg.PS3KIT_DIR "tools"
$Spu   = Join-Path $Root "src\spu"
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $Root $Out }
if ([IO.Path]::GetFullPath($Out).TrimEnd("\") -eq [IO.Path]::GetFullPath($Spu).TrimEnd("\")) { Write-Host "-Out must not be src\spu (use -Install)"; exit 1 }
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $Out
New-Item -ItemType Directory -Force $Out | Out-Null

if ($Plain) { Remove-Item Env:SPU_LIFT_XREGS -ErrorAction SilentlyContinue } else { $env:SPU_LIFT_XREGS = "1" }
try {
    $env:LIFT_JOBMODS_MERGE = "1"      # as relift.ps1: contiguous chunks lift as one C function
    python (Join-Path $Tools "lift_jobmods.py") --jobmods (Join-Path $Work "jobmods") --out $Out `
        --register (Join-Path $Out "jobmods_register.c") --symbol-prefix ascension `
        --extra-funcs-json (Join-Path $Root "tools\jobmods_extra_funcs.json") *> (Join-Path $Out "relift-jobmods.log")
    if ($LASTEXITCODE -ne 0) { Write-Host "job modules: lift failed ($Out\relift-jobmods.log)"; exit 1 }
    Remove-Item Env:LIFT_JOBMODS_MERGE -ErrorAction SilentlyContinue
    python (Join-Path $Tools "lift_spu_pic.py") --elf (Join-Path $Work "disc\PS3_GAME\USRDIR\EBOOT.ELF") `
        --fragments (Join-Path $Root "tools\job_fragments.json") --out $Out --register (Join-Path $Out "pic_register.c") `
        --symbol-prefix ascension *> (Join-Path $Out "relift-fragments.log")
    if ($LASTEXITCODE -ne 0) { Write-Host "fragments: lift failed ($Out\relift-fragments.log)"; exit 1 }
    python (Join-Path $Tools "build_spu_workloads.py") --images (Join-Path $Root "meta\spu") --lifted $Out `
        --out (Join-Path $Out "spu_workloads.c") --register-fn ascension_spu_register_all --constructor `
        --title ascension --relift --extra-funcs "spu_wws_pm_at_0077CA80=0xA2C,0xA68,0x3368,0x3378" *> (Join-Path $Out "relift-spu.log")
    if ($LASTEXITCODE -ne 0) { Write-Host "workload images: lift failed ($Out\relift-spu.log)"; exit 1 }
} finally {
    Remove-Item Env:SPU_LIFT_XREGS, Env:LIFT_JOBMODS_MERGE -ErrorAction SilentlyContinue
}

# The wrappers src\spu has now: "static spu_fn_desc s_fnw_<ADDR> = { <image>, ..." per wrapped function.
foreach ($f in Get-ChildItem $Spu -Directory) {
    $src = Join-Path $f.FullName "spu_recomp.c"
    if (-not (Test-Path $src)) { continue }
    $m = Select-String -Path $src -Pattern "^static spu_fn_desc s_fnw_([0-9A-F]{8}) = \{ (\d+)," -AllMatches
    if (-not $m) { continue }
    $image = $m[0].Matches[0].Groups[2].Value
    $addrs = @($m | ForEach-Object { $_.Matches[0].Groups[1].Value })
    python (Join-Path $Root "tools\spu_wrap.py") (Join-Path $Out $f.Name) $image @addrs
    if ($LASTEXITCODE -ne 0) { Write-Host "spu_wrap.py failed for $($f.Name)"; exit 1 }
}
$n = (Get-ChildItem $Out -Directory | Where-Object { Test-Path (Join-Path $_.FullName "spu_recomp.c") }).Count
Write-Host "$n SPU images in $Out ($(if ($Plain) { 'plain' } else { 'register-local (xregs)' }))"

if ($Install) {
    $bak = Join-Path $Root ("build\spu-backup-" + (Get-Date -Format "yyyyMMdd-HHmm"))
    Copy-Item -Recurse $Spu $bak
    Get-ChildItem $Out -Directory | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $Spu }
    Get-ChildItem $Out -File | Where-Object { $_.Extension -in ".c", ".json" } | Copy-Item -Destination $Spu -Force
    Write-Host "installed into src\spu (previous sources: $bak)"
}
