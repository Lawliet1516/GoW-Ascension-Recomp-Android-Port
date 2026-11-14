"""Profile the game's busiest threads when its frame rate drops.

usage: python tools/profile_on_drop.py --log build/logs/<name>.log --dbg <PS3_DEBUG file>
           [--below 8] [--after 90] [--times 2] [--threads 4]

Waits for an AscensionRecomp started after this script, tails its log, and the
first --times times a `[perf] fps=` line reads below --below (at least --after
seconds into the run, and 20 s apart) runs tools/profile_threads.py on it.
The game must run with PS3_DEBUG=<dbg file>.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def game_pid(since):
    ps = ("Get-Process AscensionRecomp -ErrorAction SilentlyContinue | "
          "ForEach-Object { '{0} {1}' -f $_.Id, ([DateTimeOffset]$_.StartTime).ToUnixTimeSeconds() }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    for line in out.split("\n"):
        f = line.split()
        if len(f) == 2 and int(f[1]) >= since - 5:
            return int(f[0]), int(f[1])
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--dbg", required=True)
    ap.add_argument("--below", type=float, default=8.0)
    ap.add_argument("--after", type=float, default=90.0)
    ap.add_argument("--times", type=int, default=2)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--map", default=os.path.join(HERE, "..", "build", "win", "AscensionRecomp.map"))
    a = ap.parse_args()

    t0 = time.time()
    pid = start = None
    while pid is None and time.time() - t0 < 90:
        pid, start = game_pid(t0)
        time.sleep(0.5)
    if pid is None:
        sys.exit("no game started")
    print(f"watching pid {pid}")
    pos, done, last = 0, 0, 0.0
    perf = re.compile(r"\[perf\] fps=([\d.]+).*rsx=(\d+)")
    while done < a.times:
        if not os.path.exists(a.log):
            time.sleep(1)
            continue
        with open(a.log, encoding="latin1", errors="replace") as f:
            f.seek(pos)
            chunk = f.read()
            pos = f.tell()
        for m in perf.finditer(chunk):
            fps, rsx = float(m.group(1)), int(m.group(2))
            now = time.time()
            if now - start >= a.after and fps < a.below and now - last > 20 and done < a.times:
                print(f"fps {fps} rsx {rsx}% at {now - start:.0f} s: profiling")
                out = os.path.splitext(a.log)[0] + f"-drop{done}.txt"
                subprocess.run([sys.executable, os.path.join(HERE, "profile_threads.py"), "--pid", str(pid),
                                "--dbg", a.dbg, "--threads", str(a.threads), "--map", a.map, "--out", out])
                done += 1
                last = time.time()
        if subprocess.run(["powershell", "-NoProfile", "-Command", f"Get-Process -Id {pid} -ErrorAction SilentlyContinue"],
                          capture_output=True).returncode != 0:
            break
        time.sleep(1)
    print("done")


if __name__ == "__main__":
    main()
