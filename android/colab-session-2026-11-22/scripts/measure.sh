#!/usr/bin/env bash
# measure.sh <task-id> <cmd...> : runs cmd, logs to $S/logs/<id>.log, appends one TSV line to $S/logs/measurements.tsv
# columns: id start wall_s user_s sys_s cpu% maxrss_single_proc_MB peak_system_used_MB(delta of MemAvailable) exit
S=/root/work/colab-station; id="$1"; shift
log="$S/logs/$id.log"; tf="$S/logs/$id.time"; pk="$S/logs/$id.peak"
[ -e "$log" ] && { log="$S/logs/$id.$(date +%s).log"; tf="$log.time"; pk="$log.peak"; }   # never overwrite earlier results
base=$(awk '/MemAvailable/{print $2}' /proc/meminfo); echo "$base" > "$pk"
( while :; do awk '/MemAvailable/{print $2}' /proc/meminfo >> "$pk"; sleep 1; done ) & sp=$!
start=$(date -Is)
/usr/bin/time -v -o "$tf" "$@" > "$log" 2>&1; rc=$?
kill $sp 2>/dev/null; wait $sp 2>/dev/null
minav=$(sort -n "$pk" | head -1)
wall=$(awk -F': ' '/Elapsed/{n=split($2,a,":"); s=0; for(i=1;i<=n;i++) s=s*60+a[i]; print s}' "$tf")
usr=$(awk -F': ' '/User time/{print $2}' "$tf"); sys=$(awk -F': ' '/System time/{print $2}' "$tf")
cpu=$(awk -F': ' '/Percent of CPU/{print $2}' "$tf"); rss=$(awk -F': ' '/Maximum resident/{printf "%d", $2/1024}' "$tf")
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n' "$id" "$start" "$wall" "$usr" "$sys" "$cpu" "$rss" $(( (base-minav)/1024 )) "$rc" >> "$S/logs/measurements.tsv"
echo "[$id] exit=$rc wall=${wall}s user=${usr}s sys=${sys}s cpu=$cpu maxrss=${rss}MB peak_sys=$(( (base-minav)/1024 ))MB log=$log"
exit $rc
