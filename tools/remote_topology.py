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


def launch(args):
    tar = source_tarball()
    try:
        for label in args.hosts:
            if label == "f177":
                state = remote(label, "test -f /tmp/fq-epyc7702p-full/run.pid && kill -0 $(cat /tmp/fq-epyc7702p-full/run.pid) 2>/dev/null", check=False)
                if state.returncode == 0:
                    print("f177: active existing full-span job; skipped")
                    continue
            out = run_dir(label)
            stage = f"/tmp/fastqueue2-topology-src-{label}-{int(time.time())}.tar.gz"
            with tar.open("rb") as src:
                cp = subprocess.run(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", str(tar), f"{HOSTS[label]}:{stage}"], text=True, capture_output=True)
            if cp.returncode:
                raise SystemExit(f"{label}: staging failed: {cp.stderr.strip()}")
            cmd = (
                f"cd {out}/src && python3 tools/run_topology_matrix.py "
                f"--max-cpus 0 --widths {args.widths} --transfers {args.transfers} "
                f"--min-sample-ms {args.min_sample_ms} --rounds {args.rounds} "
                f"--warmups {args.warmups} --3d-max-cpus {args.plot_cpus} --out {out}/artifacts"
            )
            setup = f"""set -eu
set -o pipefail
mkdir -p {out}/src
if test -e {out}/run.pid; then echo 'refuse existing output'; exit 2; fi
tar -xzf {stage} -C {out}/src
rm -f {stage}
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
    if label == "f177":
        return "/tmp/fq-epyc7702p-full"
    return remote(label, "ls -dt /tmp/fq-topology-" + label + "-* 2>/dev/null | head -1", check=False).stdout.strip()


def job_artifacts_dir(label, directory):
    return directory if label == "f177" else f"{directory}/artifacts"


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
            local(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"{HOSTS[label]}:{d}/launch.json", f"{HOSTS[label]}:{d}/command.txt", str(target)])
        print(f"{label}: harvested {target}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=("launch", "status", "harvest"))
    p.add_argument("--hosts", nargs="+", choices=sorted(HOSTS), default=sorted(HOSTS))
    p.add_argument("--transfers", type=int, default=720720)
    p.add_argument("--min-sample-ms", type=int, default=100)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--warmups", type=int, default=1)
    p.add_argument("--plot-cpus", type=int, default=0)
    p.add_argument("--widths", default="0", help="comma-separated modes; default scalar only")
    a = p.parse_args()
    {"launch": launch, "status": status, "harvest": harvest}[a.action](a)

if __name__ == "__main__": main()
