#!/usr/bin/env python3
"""Collect every non-Starfish series used by the paper auto-scale figures."""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PAPER_TIGON_REV = "16f8007fa15bc853397b04e0747efc4f8c21ef25"
MACHINES = [1, 2, 4, 6, 8]


def run(cmd, *, cwd=None, log=None, env=None):
    print("+", " ".join(map(str, cmd)), flush=True)
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=1)
    lines = []
    assert proc.stdout is not None
    for line in proc.stdout:
        print(line, end="", flush=True)
        lines.append(line)
    proc.wait()
    output = "".join(lines)
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(output)
    if proc.returncode:
        raise SystemExit(f"command failed ({proc.returncode}); see {log}: {' '.join(map(str, cmd))}")
    return output


def require_tools(*names):
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise SystemExit("missing host tools: " + ", ".join(missing))


def average(values, label):
    if not values:
        raise SystemExit(f"could not extract {label}")
    return sum(values) / len(values)


def append_result(path, app, series, n, value, unit):
    with path.open("a") as out:
        out.write(f"\nAE_RESULT app={app} series={series} n={n} value={value:.9f} unit={unit}\n")


def collect_linux(log_dir: Path, graph: Path):
    require_tools("cmake", "make", "mpicxx", "mpirun", "numactl")
    phoenix = ROOT / "test-on-linux/phoenix"
    db = ROOT / "test-on-linux/dbx1000"
    gemini = ROOT / "test-on-linux/GeminiGraph"
    distributed = ROOT / "test-on-linux/ggraph-distri"
    for tree in (phoenix, db, gemini, distributed):
        if not tree.exists():
            raise SystemExit(f"missing Linux baseline submodule: {tree}")
    if not graph.is_file():
        raise SystemExit(f"missing twitter graph: {graph}; rerun prepare.sh without SKIP_GRAPH_DATASET=1")

    run(["cmake", "-S", str(phoenix), "-B", str(phoenix / "build"), "-DBREAKDOWN=ON"])
    run(["cmake", "--build", str(phoenix / "build"), "-j"])
    matrix = phoenix / "build/bin/matrix_multiply"

    run(["cmake", "-S", str(gemini), "-B", str(gemini / "build")])
    run(["cmake", "--build", str(gemini / "build"), "-j"])
    run(["make", "-j"], cwd=distributed)

    config = db / "config.h"
    original = config.read_text()
    configured = re.sub(r"(#define\s+WORKLOAD\s+)\w+", r"\1TPCC", original)
    configured = re.sub(r"(#define\s+NUM_WH\s+)\d+", r"\g<1>64", configured)
    config.write_text(configured)
    try:
        run(["make", "clean"], cwd=db)
        run(["make", "-j"], cwd=db)
        matrix_data = phoenix / "data/matrix_datafiles"
        bind = matrix_data / "matrix_multiply_bind_cpu.txt"
        bind.write_text("0-95\n")
        for n in MACHINES:
            app_threads = 8 * n
            graph_threads = 12 * n
            path = log_dir / f"matrix_Ideal_N{n}.log"
            text = run([str(matrix), "-l", "4000", "-r", "4000", "-t", str(app_threads), "-c", "0"],
                       cwd=matrix_data, log=path)
            us = average([float(x) for x in re.findall(r"(?<!inter )library:\s*([\d.]+)", text)], path.name)
            append_result(path, "matrix", "Ideal", n, us, "us")

            path = log_dir / f"db1000_Ideal_N{n}.log"
            text = run([str(db / "rundb"), f"-t{app_threads}", "-n64", "-Tp0.5"], cwd=db, log=path)
            summaries = re.findall(r"\[summary\]\s+txn_cnt=(\d+).*?run_time=([\d.]+)", text)
            if not summaries:
                raise SystemExit(f"could not extract DBx1000 summary from {path}")
            txn, runtime = summaries[-1]
            mops = float(txn) / float(runtime) * app_threads / 1e6
            append_result(path, "db1000", "Ideal", n, mops, "mops")

            path = log_dir / f"gemini_Ideal_N{n}.log"
            text = run(["numactl", "--membind", "0", str(gemini / "build/pagerank"),
                        str(graph), "41652230", "50", str(graph_threads),
                        str(graph_threads)], log=path)
            times = [float(x) for x in re.findall(r"exec_time=([\d.]+)\(s\)", text)]
            secs = average(times[1:] or times, path.name)
            append_result(path, "gemini", "Ideal", n, secs, "s")

            path = log_dir / f"gemini_Distributed_N{n}.log"
            mpi_env = os.environ.copy()
            mpi_env["OMP_NUM_THREADS"] = "12"
            mpi_env["OMP_PROC_BIND"] = "true"
            text = run(["mpirun", "--map-by", "slot:PE=12", "--bind-to", "core",
                        "-np", str(n), str(distributed / "toolkits/pagerank"),
                        str(graph), "41652230", "50"], log=path, env=mpi_env)
            times = [float(x) for x in re.findall(r"exec_time=([\d.]+)\(s\)", text)]
            secs = average(times[1:] or times, path.name)
            append_result(path, "gemini", "Distributed", n, secs, "s")
    finally:
        config.write_text(original)


def collect_matrix_tcp(log_dir: Path):
    require_tools("mpicc", "mpirun")
    binary = log_dir.parent / "matrix_tcp_mpi"
    run(["mpicc", "-O3", "-fopenmp", str(HERE / "matrix_tcp_mpi.c"), "-o", str(binary)])
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = os.environ.get("MATRIX_TCP_THREADS", "8")
    for n in MACHINES:
        path = log_dir / f"matrix_Distributed_N{n}.log"
        text = run(["mpirun", "--bind-to", "none", "--mca", "btl", "tcp,self",
                    "--mca", "btl_tcp_if_include", "lo", "-np", str(n),
                    str(binary), "4000"], log=path, env=env)
        match = re.search(r"TIME=(\d+)", text)
        if not match:
            raise SystemExit(f"could not extract TCP Matrix time from {path}")
        append_result(path, "matrix", "Distributed", n, float(match.group(1)), "us")


def ensure_tigon(path: Path, log_dir: Path):
    """Validate and record the supplied Tigon tree without modifying it."""
    require_tools("git")
    required = ("scripts/run.sh", "scripts/setup.sh",
                "emulation/image/make_vm_img.sh", "emulation/start_vms.sh")
    missing = [name for name in required if not (path / name).is_file()]
    if missing:
        raise SystemExit(
            f"invalid Tigon checkout {path}; missing: {', '.join(missing)}. "
            "Set TIGON_DIR to the existing Tigon source tree."
        )

    def git_output(*args):
        proc = subprocess.run(["git", *args], cwd=path, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode:
            raise SystemExit(f"cannot inspect Tigon checkout {path}: {proc.stderr.strip()}")
        return proc.stdout.strip()

    head = git_output("rev-parse", "HEAD")
    status = git_output("status", "--short")
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", PAPER_TIGON_REV, "HEAD"],
        cwd=path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    metadata = [
        f"tigon_dir={path.resolve()}",
        f"head={head}",
        f"paper_revision={PAPER_TIGON_REV}",
        f"paper_revision_is_ancestor={int(ancestor)}",
        f"worktree_dirty={int(bool(status))}",
    ]
    if status:
        metadata.extend(("", "git_status:", status))
    log_dir.mkdir(parents=True, exist_ok=True)
    (log_dir / "tigon-source.txt").write_text("\n".join(metadata) + "\n")
    print(f"[AE] Using Tigon checkout {path.resolve()} at {head[:12]}", flush=True)
    if not ancestor:
        print(f"[AE] WARNING: paper Tigon revision {PAPER_TIGON_REV[:12]} "
              "is not an ancestor of this checkout", flush=True)
    if status:
        print("[AE] Tigon worktree has local changes; preserving and using them", flush=True)


def collect_tigon(log_dir: Path, tigon: Path, setup: bool):
    ensure_tigon(tigon, log_dir)
    if setup:
        run(["bash", "scripts/setup.sh", "HOST"], cwd=tigon)
        run(["bash", "emulation/image/make_vm_img.sh"], cwd=tigon)
        run(["sudo", "bash", "emulation/start_vms.sh", "--using-old-img", "--cxl",
             "0", "12", "8", "1", "1"], cwd=tigon)
        run(["bash", "scripts/setup.sh", "VMS", "8"], cwd=tigon)
        run(["bash", "scripts/run.sh", "COMPILE_SYNC", "8"], cwd=tigon)
    for n in MACHINES:
        path = log_dir / f"db1000_Tigon_N{n}.log"
        cmd = ["bash", "scripts/run.sh", "TPCC", "TwoPLPasha", str(n), "3",
               "mixed", "10", "15", "1", "0", "1", "Clock", "OnDemand",
               "200000000", "1", "WriteThrough", "None", "30", "10",
               "BLACKHOLE", "0", "0", "0"]
        text = run(cmd, cwd=tigon, log=path)
        vals = [float(x) for x in re.findall(r"Global Stats:.*?total_commit(?:\(TPS\))?:\s*([\d.]+)", text)]
        append_result(path, "db1000", "Tigon", n, average(vals, path.name) / 1e6, "mops")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", required=True, type=Path)
    ap.add_argument("--stages", default="linux,matrix-tcp,tigon")
    ap.add_argument("--graph", type=Path, default=ROOT / "datasets/twitter-2010.bin")
    ap.add_argument("--tigon-dir", type=Path,
                    default=ROOT / "artifact-evaluation/deps/tigon",
                    help="Tigon source tree (default: AE submodule ../deps/tigon)")
    ap.add_argument("--skip-tigon-setup", action="store_true",
                    help="reuse already-running Tigon VMs and synchronized binary")
    args = ap.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    stages = {x.strip() for x in args.stages.split(",") if x.strip()}
    unknown = stages - {"linux", "matrix-tcp", "tigon"}
    if unknown:
        ap.error("unknown stages: " + ", ".join(sorted(unknown)))
    if "linux" in stages:
        collect_linux(args.log_dir, args.graph)
    if "matrix-tcp" in stages:
        collect_matrix_tcp(args.log_dir)
    if "tigon" in stages:
        collect_tigon(args.log_dir, args.tigon_dir, not args.skip_tigon_setup)


if __name__ == "__main__":
    main()
