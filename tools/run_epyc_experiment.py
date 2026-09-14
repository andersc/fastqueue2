#!/usr/bin/env python3
"""Build and run reproducible FastQueue2 EPYC slot-signaling experiments."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import pathlib
import platform
import shlex
import subprocess
import sys
from typing import Any

ATOMIC_QUEUE_REPOSITORY = "https://github.com/max0x7ba/atomic_queue.git"
ATOMIC_QUEUE_REVISION = "c3eeb3d7bf85e8a6414294f5510681b7a595f97c"  # v1.9.2


def run(command: list[str], *, cwd: pathlib.Path | None = None,
        output: pathlib.Path | None = None) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(command), flush=True)
    if output:
        with output.open("w", encoding="utf-8") as stream:
            return subprocess.run(command, cwd=cwd, check=True, text=True,
                                  stdout=stream, stderr=subprocess.STDOUT)
    return subprocess.run(command, cwd=cwd, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def ensure_atomic_queue(source_dir: pathlib.Path) -> pathlib.Path:
    checkout = source_dir / "atomic_queue"
    if not checkout.exists():
        run(["git", "clone", "--filter=blob:none", ATOMIC_QUEUE_REPOSITORY,
             str(checkout)])
    run(["git", "fetch", "--quiet", "origin", ATOMIC_QUEUE_REVISION], cwd=checkout)
    run(["git", "checkout", "--quiet", "--detach", ATOMIC_QUEUE_REVISION], cwd=checkout)
    run(["git", "reset", "--quiet", "--hard", ATOMIC_QUEUE_REVISION], cwd=checkout)
    run(["git", "clean", "--quiet", "-dffx"], cwd=checkout)
    actual = run(["git", "rev-parse", "HEAD"], cwd=checkout).stdout.strip()
    if actual != ATOMIC_QUEUE_REVISION:
        raise RuntimeError(f"AtomicQueue revision mismatch: {actual}")
    return checkout


def source_identity(source: pathlib.Path) -> dict[str, Any]:
    tracked = ["fast_queue_x86_64.h", "fast_queue_x86_64_epyc.h",
               "FastQueueEpycExperiment.cpp"]
    hashes = {
        name: __import__("hashlib").sha256((source / name).read_bytes()).hexdigest()
        for name in tracked
    }
    try:
        revision = run(["git", "rev-parse", "HEAD"], cwd=source).stdout.strip()
        status = run(["git", "status", "--short"], cwd=source).stdout
        diff = run(["git", "diff", "--no-ext-diff", "--binary", "--",
                    "fast_queue_x86_64.h", "fast_queue_x86_64_epyc.h",
                    "FastQueueEpycExperiment.cpp"], cwd=source).stdout
        return {
            "revision": revision,
            "dirty": bool(status.strip()),
            "status_short": status.splitlines(),
            "experiment_diff": diff,
            "sha256": hashes,
        }
    except (subprocess.CalledProcessError, FileNotFoundError):
        return {"revision": None, "dirty": None, "sha256": hashes}


def cpu_identity() -> dict[str, Any]:
    identity: dict[str, Any] = {
        "machine": platform.machine(),
        "processor": platform.processor(),
    }
    if platform.system() == "Linux":
        try:
            identity["lscpu"] = run(["lscpu", "-J"]).stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
    return identity


def topology_relation(cpu_a: int, cpu_b: int) -> dict[str, Any]:
    result: dict[str, Any] = {"producer_cpu": cpu_a, "consumer_cpu": cpu_b}
    if platform.system() != "Linux":
        return result
    for cpu, prefix in ((cpu_a, "producer"), (cpu_b, "consumer")):
        root = pathlib.Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        for name in ("physical_package_id", "core_id"):
            path = root / name
            if path.exists():
                result[f"{prefix}_{name}"] = int(path.read_text().strip())
    a_pkg = result.get("producer_physical_package_id")
    b_pkg = result.get("consumer_physical_package_id")
    a_core = result.get("producer_core_id")
    b_core = result.get("consumer_core_id")
    if a_pkg is not None and b_pkg is not None:
        if a_pkg != b_pkg:
            result["relation"] = "cross_socket"
        elif a_core == b_core:
            result["relation"] = "smt_sibling"
        else:
            result["relation"] = "same_socket"
    return result


def parse_summary(path: pathlib.Path, capacity: int, producer: int,
                  consumer: int) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    marker = "summary,queue,median_mitems_s,min_mitems_s,max_mitems_s,cv_pct"
    lines = path.read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index(marker) + 1
    except ValueError as error:
        raise RuntimeError(f"summary marker missing in {path}") from error
    relation = topology_relation(producer, consumer)
    for row in csv.reader(lines[start:]):
        if len(row) != 5:
            continue
        rows.append({
            "capacity": capacity,
            **relation,
            "queue": row[0],
            "median_mitems_s": float(row[1]),
            "min_mitems_s": float(row[2]),
            "max_mitems_s": float(row[3]),
            "cv_pct": float(row[4]),
        })
    if len(rows) != 3:
        raise RuntimeError(f"expected 3 summaries in {path}, got {len(rows)}")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--work", type=pathlib.Path,
                        default=pathlib.Path("/tmp/fq-epyc-experiment"))
    parser.add_argument("--capacities", default="64,256,1024,4096,65536")
    parser.add_argument("--placements", default="1:3",
                        help="comma-separated producer:consumer CPU pairs")
    parser.add_argument("--transfers", type=int, default=50_000_000)
    parser.add_argument("--rounds", type=int, default=12)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--march", default="native")
    parser.add_argument("--perf", action="store_true")
    parser.add_argument("--assembly", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    atomic = ensure_atomic_queue(work)
    capacities = [int(value) for value in args.capacities.split(",")]
    placements = [tuple(map(int, value.split(":")))
                  for value in args.placements.split(",")]
    include = atomic / "include"
    all_rows: list[dict[str, Any]] = []

    metadata = {
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "atomic_queue_repository": ATOMIC_QUEUE_REPOSITORY,
        "atomic_queue_revision": ATOMIC_QUEUE_REVISION,
        "compiler": args.compiler,
        "compiler_version": run([args.compiler, "--version"]).stdout.splitlines()[0],
        "march": args.march,
        "transfers": args.transfers,
        "rounds": args.rounds,
        "capacities": capacities,
        "placements": placements,
        "uname": platform.uname()._asdict(),
        "cpu_identity": cpu_identity(),
        "fastqueue2_source": source_identity(source),
        "note": "AtomicQueue template uses the same requested power-of-two capacity; FastQueue2 mask equals capacity minus one.",
    }
    (work / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    for capacity in capacities:
        command = [
            args.compiler, "-std=c++20", "-O3", "-DNDEBUG",
            f"-march={args.march}", "-pthread",
            f"-DEXPERIMENT_CAPACITY={capacity}",
            f"-DEXPERIMENT_TRANSFERS={args.transfers}",
            f"-DEXPERIMENT_ROUNDS={args.rounds}",
            f"-I{source}", f"-I{include}",
            str(source / "FastQueueEpycExperiment.cpp"),
        ]
        if args.assembly:
            asm = work / f"epyc-experiment-cap-{capacity}.s"
            run(command + ["-S", "-fverbose-asm", "-o", str(asm)])

        for producer, consumer in placements:
            placed_binary = work / f"run-cap-{capacity}-p{producer}-c{consumer}"
            placed_command = command + [
                f"-DEXPERIMENT_PRODUCER_CPU={producer}",
                f"-DEXPERIMENT_CONSUMER_CPU={consumer}",
                "-o", str(placed_binary),
            ]
            run(placed_command)
            log = work / f"cap-{capacity}-p{producer}-c{consumer}.log"
            run([str(placed_binary)], output=log)
            all_rows.extend(parse_summary(log, capacity, producer, consumer))

            if args.perf:
                perf_log = work / f"perf-cap-{capacity}-p{producer}-c{consumer}.log"
                run(["perf", "stat", "-x,", "-e",
                     "cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations",
                     str(placed_binary)], output=perf_log)

    result_path = work / "results.csv"
    fieldnames = sorted({key for row in all_rows for key in row})
    with result_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(all_rows)
    print(f"wrote {result_path} ({len(all_rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
