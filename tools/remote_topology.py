#!/usr/bin/env python3
"""Launch, inspect, and harvest detached Linux topology jobs.

Each job receives a source tarball from current checkout, builds remotely, then
runs under nohup. SSH disconnects or local-machine reboots do not stop jobs.
Never launch over an existing output directory: benchmark CSV output truncates.
"""
from __future__ import annotations
import argparse, json, shlex, subprocess, sys, tarfile, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOSTS = {
    "f131": "anders.cedronius@f131-lab-ac.lab.tickup.net",
    "f177": "s05u24-f177-lab.infra.tickup.io",
    "f061": "anders.cedronius@f061-lab-gpu.lab.tickup.net",
}
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12"]


def local(args, **kw):
    print("+", " ".join(map(str, args)))
    return subprocess.run(args, check=True, text=True, **kw)


def remote(host, script, check=True):
    return subprocess.run(SSH + [HOSTS[host], "bash", "-s"], input=script, check=check,
                          text=True, capture_output=True)


def source_tarball():
    fd, name = tempfile.mkstemp(prefix="fastqueue2-topology-", suffix=".tar.gz")
    Path(name).unlink(missing_ok=True)
    with tarfile.open(name, "w:gz") as tar:
        for p in ROOT.rglob("*"):
            rel = p.relative_to(ROOT)
            if not p.is_file() or any(part == ".git" or part.startswith("cmake-build") or part == "__pycache__" for part in rel.parts):
                continue
            tar.add(p, arcname=str(rel), recursive=False)
    return Path(name)


def run_dir(label):
    return f"/tmp/fq-topology-{label}-{time.strftime('%Y%m%d-%H%M%S')}"


def isolation_preflight_script(out: str, seconds: int) -> str:
    """Record host activity before benchmark starts; never claim OS isolation."""
    return f'''set -eu
out={shlex.quote(out)}
python3 - {shlex.quote(out)} {seconds} <<'PY'
import json, os, pathlib, subprocess, sys, time
out, seconds = sys.argv[1], int(sys.argv[2])
def cpu_times():
    rows = {{}}
    for line in pathlib.Path('/proc/stat').read_text().splitlines():
        fields = line.split()
        if not fields or not fields[0].startswith('cpu') or fields[0] == 'cpu':
            continue
        try:
            values = [int(x) for x in fields[1:]]
        except ValueError:
            continue
        total = sum(values); idle = values[3] + (values[4] if len(values) > 4 else 0)
        rows[fields[0][3:]] = (total, idle)
    return rows
before = cpu_times(); time.sleep(seconds); after = cpu_times()
activity = {{}}
for cpu, (total0, idle0) in before.items():
    total1, idle1 = after.get(cpu, (total0, idle0)); delta = total1 - total0
    activity[cpu] = 0.0 if delta <= 0 else round(100.0 * (delta - (idle1 - idle0)) / delta, 3)
def capture(cmd):
    p = subprocess.run(cmd, text=True, capture_output=True)
    return p.stdout if p.returncode == 0 else ''
processes = []
for line in capture(['ps', '-eLo', 'pid,ppid,user,psr,pcpu,stat,comm,args', '--no-headers']).splitlines():
    fields = line.split(None, 7)
    if len(fields) >= 7:
        processes.append({{'pid': fields[0], 'ppid': fields[1], 'user': fields[2], 'cpu': fields[3], 'cpu_pct': fields[4], 'state': fields[5], 'command': fields[6], 'args': fields[7] if len(fields) == 8 else ''}})
data = {{
  'schema': 1,
  'captured_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
  'sample_seconds': seconds,
  'per_cpu_busy_pct': activity,
  'processes': processes,
  'loadavg': pathlib.Path('/proc/loadavg').read_text().strip(),
  'online_cpus': pathlib.Path('/sys/devices/system/cpu/online').read_text().strip(),
  'governor': {{p.name: p.read_text().strip() for p in pathlib.Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_governor')}},
  'irq_affinity': {{p.name: p.read_text().strip() for p in pathlib.Path('/proc/irq').glob('*/smp_affinity_list') if p.is_file()}},
  'controls': [
    'benchmark workers use sched_setaffinity hard logical-CPU pinning',
    'preflight records host activity and runnable processes before launch',
    'no claim of IRQ, kernel-work, thermal, or frequency isolation'
  ]
}}
pathlib.Path(out).write_text(json.dumps(data, indent=2, sort_keys=True) + '\\n')
PY
'''


def launch(args):
    tar = source_tarball()
    try:
        for label in args.hosts:
            out = run_dir(label)
            stage = f"/tmp/fastqueue2-topology-src-{label}-{int(time.time())}.tar.gz"
            with tar.open("rb") as src:
                cp = subprocess.run(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", str(tar), f"{HOSTS[label]}:{stage}"], text=True, capture_output=True)
            if cp.returncode:
                raise SystemExit(f"{label}: staging failed: {cp.stderr.strip()}")
            widths_arg = f"--widths {shlex.quote(args.widths)} " if args.widths else ""
            cmd = (
                f"cd {out}/src && python3 tools/run_topology_matrix.py "
                f"--max-cpus 0 {widths_arg}--transfers {args.transfers} "
                f"--min-sample-ms {args.min_sample_ms} --rounds {args.rounds} "
                f"--warmups {args.warmups} --3d-max-cpus {args.plot_cpus} --out {out}/artifacts"
            )
            setup = f"""set -eu
set -o pipefail
mkdir -p {out}/src
if test -e {out}/run.pid; then echo 'refuse existing output'; exit 2; fi
tar -xzf {stage} -C {out}/src
rm -f {stage}
{isolation_preflight_script(out + '/isolation-preflight.json', args.preflight_seconds)}
cd {out}/src
if command -v cmake >/dev/null 2>&1; then
  cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
  cmake --build cmake-build-release --target fast_queue_topology_matrix -j$(nproc)
else
  mkdir -p cmake-build-release
  c++ -std=c++20 -O3 -DNDEBUG -pthread -I. FastQueueTopologyMatrix.cpp -o cmake-build-release/fast_queue_topology_matrix
fi
printf '%s\\n' {shlex.quote(cmd)} > {out}/command.txt
printf '%s\\n' '{json.dumps({'host': label, 'ssh': HOSTS[label], 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'source_revision': subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(), 'config': vars(args)}, sort_keys=True)}' > {out}/launch.json
nohup bash -lc {shlex.quote(cmd)} > {out}/run.log 2>&1 < /dev/null &
echo $! > {out}/run.pid
echo {out}
"""
            result = remote(label, setup)
            print(f"{label}: launched {result.stdout.strip()}")
    finally:
        tar.unlink(missing_ok=True)


def job_dir(label):
    return remote(label, "ls -dt /tmp/fq-topology-" + label + "-* 2>/dev/null | head -1", check=False).stdout.strip()


def job_artifacts_dir(label, directory):
    return f"{directory}/artifacts"


def status(args):
    for label in args.hosts:
        d = job_dir(label)
        if not d:
            print(f"{label}: no orchestrated run")
            continue
        artifacts = job_artifacts_dir(label, d)
        script = f"""d={d}; a={artifacts}; pid=$(cat $d/run.pid 2>/dev/null || true); echo DIR=$d; echo PID=$pid
if test -n \"$pid\" && kill -0 $pid 2>/dev/null; then echo STATE=running; else echo STATE=finished_or_failed; fi
wc -l $a/results.csv 2>/dev/null || true
tail -1 $d/run.log 2>/dev/null || true
"""
        r = remote(label, script, check=False)
        print(f"[{label}]\n{r.stdout.strip()}\n{r.stderr.strip()}")


def harvest(args):
    dest = ROOT / "docs" / "topology-matrix" / "linux-runs"
    dest.mkdir(parents=True, exist_ok=True)
    for label in args.hosts:
        d = job_dir(label)
        if not d:
            print(f"{label}: no run")
            continue
        artifacts = job_artifacts_dir(label, d)
        pid = remote(label, f"cat {d}/run.pid 2>/dev/null", check=False).stdout.strip()
        alive = remote(label, f"kill -0 {pid} 2>/dev/null", check=False).returncode == 0 if pid else False
        ready = remote(label, f"test -f {artifacts}/results.csv && test -f {artifacts}/summary.json && test -f {artifacts}/metadata.json", check=False).returncode == 0
        if alive or not ready:
            print(f"{label}: skip (state={'running' if alive else 'incomplete'}, dir={d})")
            continue
        target = dest / Path(d).name
        target.mkdir(exist_ok=True)
        local(["scp", "-r", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"{HOSTS[label]}:{artifacts}/.", str(target)])
        if label == "f177":
            local(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"{HOSTS[label]}:{d}/run.log", f"{HOSTS[label]}:{d}/run.pid", str(target)])
        else:
            local(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"{HOSTS[label]}:{d}/launch.json", f"{HOSTS[label]}:{d}/command.txt", f"{HOSTS[label]}:{d}/isolation-preflight.json", str(target)])
        print(f"{label}: harvested {target}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=("launch", "status", "harvest"))
    p.add_argument("--hosts", nargs="+", choices=sorted(HOSTS), default=sorted(HOSTS))
    p.add_argument("--transfers", type=int, default=720720)
    p.add_argument("--min-sample-ms", type=int, default=100)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--warmups", type=int, default=1)
    p.add_argument("--preflight-seconds", type=int, default=10,
                   help="seconds of host activity captured before remote launch")
    p.add_argument("--plot-cpus", type=int, default=0)
    p.add_argument("--widths", default="", help="comma-separated modes; empty runs all supported widths (0=scalar)")
    a = p.parse_args()
    {"launch": launch, "status": status, "harvest": harvest}[a.action](a)

if __name__ == "__main__": main()
