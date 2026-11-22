# Memory planning and a memory watchdog for the lifted-code compile (sourced by build_lifted.sh, and by tests/test_mem_guard.sh).
#
# Each lifted PPU chunk takes 2-4 GB in clang (the Windows build's numbers; not yet measured on Android/clang 21). A machine without swap
# (Google Colab: 12.67 GB) loses the whole build to the kernel's OOM killer, which may kill any process. So:
#   - 1 job unless a MEASUREMENT from an earlier build in the same build directory shows room for more (mem_plan_jobs);
#   - the build runs under a watchdog that stops it in an orderly way when available memory falls below a floor (mem_guard_run).

mem_avail_mb() { awk '/MemAvailable/ {printf "%d", $2 / 1024}' /proc/meminfo; }

# mem_plan_jobs <requested jobs> <measured peak MB per compile, or empty> <available MB> <reserve MB>
# prints the job count to use, or explains on stderr and returns 1. One job needs no measurement (it is the safe default) but the
# available memory must cover a pessimistic 4 GB compile + the reserve; more jobs need jobs * peak * 1.25 + reserve <= available.
mem_plan_jobs() {
  local want="$1" peak="$2" avail="$3" reserve="$4"
  [[ "$want" =~ ^[1-9][0-9]*$ ]] || { echo "LIFT_JOBS must be a positive integer (got '$want')" >&2; return 1; }
  if [ "$want" = 1 ]; then
    local need=$((4096 + reserve))
    [ "$avail" -ge "$need" ] || { echo "only ${avail} MB available; one lifted-file compile may need 4096 MB + ${reserve} MB reserve. Free memory or use a bigger machine." >&2; return 1; }
    echo 1; return 0
  fi
  [ -n "$peak" ] && [ "$peak" -gt 0 ] 2>/dev/null || { echo "LIFT_JOBS=$want refused: no measured per-compile peak yet. Build once with LIFT_JOBS=1 in this build directory (it records the peak), then retry." >&2; return 1; }
  local need=$(( want * peak * 5 / 4 + reserve ))
  [ "$avail" -ge "$need" ] || { echo "LIFT_JOBS=$want refused: measured peak ${peak} MB per compile x $want x 1.25 + ${reserve} MB reserve = ${need} MB > ${avail} MB available." >&2; return 1; }
  echo "$want"
}

# mem_tree <pid>: the pid and all its descendants (collected BEFORE signalling: once a parent dies its children are re-parented and the
# link is lost). Process groups are not enough: ninja puts every compiler it starts in a process group of its own.
mem_tree() {
  local q="$1" all="$1" p c
  while [ -n "$q" ]; do
    set -- $q; p="$1"; shift; q="$*"
    for c in $(ps -o pid= --ppid "$p" 2>/dev/null); do all="$all $c"; q="$q $c"; done
  done
  echo "$all"
}

# mem_guard_run <floor MB> <log tsv> <cmd...>: runs cmd, samples every second
#   time  MemAvailable_MB  compile processes  sum RSS MB  largest single RSS MB  (compile processes = clang/cc1/ld.lld)
# If MemAvailable drops below the floor, the command's whole process tree gets SIGTERM (ninja then stops its jobs itself), and SIGKILL
# whatever is still alive 10 s later; the function returns 75. Otherwise it returns the command's exit code.
# The largest single-process RSS seen is written to <log>.peak (MB). Sampling is once a second: a compile shorter than that can be missed
# (peak 0 = no measurement, never "needs nothing": see mem_plan_jobs).
mem_guard_run() {
  local floor="$1" log="$2"; shift 2
  printf 't_s\tavail_mb\tnproc\tsum_rss_mb\tmax_rss_mb\n' > "$log"
  "$@" & local pid=$! t0=$SECONDS peak=0 stopped=0
  while kill -0 "$pid" 2>/dev/null; do
    local a s; a="$(mem_avail_mb)"
    s="$(ps -eo rss=,comm= | awk '$2 ~ /^(clang|clang\+\+|clang-[0-9]+|cc1|cc1plus|ld\.lld|ld)$/ {n++; t+=$1; if ($1 > m) m = $1} END {printf "%d %d %d", n, t/1024, m/1024}')"
    set -- $s; [ "$3" -gt "$peak" ] && peak="$3"
    printf '%d\t%d\t%d\t%d\t%d\n' $((SECONDS - t0)) "$a" "$1" "$2" "$3" >> "$log"
    if [ "$a" -lt "$floor" ]; then
      echo "MEMORY GUARD: ${a} MB available < floor ${floor} MB: stopping the build (orderly, before the kernel's OOM killer)." >&2
      local tree; tree="$(mem_tree "$pid")"; stopped=1
      kill -TERM $tree 2>/dev/null
      local w=0   # alive = not a zombie (our own child stays a zombie until wait, and kill -0 succeeds on zombies)
      while [ $w -lt 10 ] && ps -o stat= -p "$(echo $tree | tr ' ' ',')" 2>/dev/null | grep -qv '^Z'; do sleep 1; w=$((w + 1)); done
      kill -KILL $tree 2>/dev/null
      break
    fi
    sleep 1
  done
  wait "$pid"; local rc=$?
  echo "$peak" > "$log.peak"
  [ $stopped = 1 ] && return 75
  return $rc
}
