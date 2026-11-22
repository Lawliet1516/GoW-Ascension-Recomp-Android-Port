# Colab session of 2026-11-22: audit, wave 1 and Phase A (paused)

Work done on a Google Colab VM (2 vCPU Xeon 2.2 GHz, 12.67 GB RAM, no swap, Ubuntu 24.04), starting from commit `dc38f15`
(`android/lift-pipeline`). No game file (EBOOT, ISO, lifted code) was ever present on that machine; nothing here is derived from the game.

## Where things stand (paused)

| Phase | State |
|---|---|
| Audit of the lift pipeline and the environment | done: `reports/relatorio-auditoria-e-rodada1.md` (PDF beside it) |
| Wave 1 (tasks without the EBOOT: tests, conformance, runtime build, static analysis) | done: `reports/REPORT-wave1.md`, `results/` |
| Phase A (build infrastructure, items A1-A5) | done and committed on `colab/phase-a-fixes` (5 commits after `dc38f15`): `reports/relatorio-faseA.md` |
| Phase B (Linux/Android compatibility: 30 failing Python tests, `test_ppu_lift.py` on POSIX, 17 undefined runtime symbols) | not started |
| Phase C (triage of the 37 clang-analyzer findings) | not started |
| Phase D (APK of `m3-app-swapchain`, Linux ps3kit + CTest, renderer scene runner build) | not started |
| Phases E-G (real lift, lifted-code build, device validation) | blocked: EBOOT and the owner's authorization to use it on Colab |

## Decisions pending from the owner

1. Push of `colab/phase-a-fixes` to GitHub (nothing has been pushed).
2. B2: `PPU_LEAN_CONST` belongs to ps3kit; look for a fix outside ps3kit first, and ask before changing `ps3kit/tools`.
3. Whether the EBOOT may be used on Colab / Google Drive (project rules on third-party services).

## Known open issues (found, not fixed)

- `android/lift/tests/test_mem_guard.sh`, check "its RSS was measured (> 50 MB)": **intermittent** (failed 1 of 4 runs at the end of the
  session). The synthetic compile lasts about 1 s and the watchdog samples once a second, so a run can see no compiler process. The
  mechanism itself measured 104-112 MB in the other runs. Fix on resumption: make the synthetic compile longer, or sample faster in the test.
- Step 1 of `prepare.sh` (job-module carve) is still gated on one file; `ascension/scripts/windows/prepare.ps1` has the step-3 flaw fixed
  here for Linux (not changed: cannot be tested on Linux).
- Phase A residual risks: see section 5 of `reports/relatorio-faseA.md`.

## Contents

- `reports/`: the three reports (Markdown) and their PDFs.
- `results/`: `measurements.tsv` (wall time, CPU, peak RAM of every run), T1 summary and failure classification, T2 conformance log
  (1533/1533 on x86 and arm64-under-qemu), T3 undefined runtime symbols, T4 VMX/NEON bit-exactness log, T6 analyzer findings.
- `scripts/`: the session's helpers (`measure.sh` time/RAM wrapper, test runners, `t6_tidy.sh` resumable clang-tidy batch, dry-run state
  diff, `topdf.py`). They use absolute paths under `/root/work/colab-station` (the Colab workspace); adjust `S=` before reuse.

## Not in the repository (and why)

- The Colab workspace `/root/work/colab-station` (NDK r29 2.4 GB, SDL3 build, venvs, full build trees and logs, the 3.8 MB results archive
  `results-wave1-dc38f15.tar.gz`): toolchains and build outputs are reproducible and too large; the archive's useful parts are in
  `results/`. It disappears when the Colab VM is recycled.
- Nothing derived from the game exists anywhere; `.gitignore` covers `work/`, `ascension/src/` and now `build-lifted*/`.

## Resuming on a fresh Colab VM

```sh
git checkout colab/phase-a-fixes
# toolchain (about 3 min): ninja, qemu-user, clang, a venv with ps3kit/requirements.txt, NDK r29:
#   curl -fsSLO https://dl.google.com/android/repository/android-ndk-r29-linux.zip   (29.0.14206865)
android/lift/tests/test_stage.sh                       # 38 checks
NDK=<ndk> android/lift/tests/test_mem_guard.sh         # 33 checks (see the intermittent one above)
NDK=<ndk> OUT=/outside/the/repo android/lift/selftest.sh
```
