#!/usr/bin/env python3
"""One-click artifact evaluation — thin wrapper over per-experiment run.sh.

From the repository root (after submodule init + install-host-deps)::

    python3 artifact-evaluation/run_all.py          # default: ready experiments
    ./artifact-evaluation/run-all.sh

Flow:
  1. ``artifact-evaluation/prepare.sh`` (unless ``--no-prepare``)
     — submodule check, dataset download, ivshmem / hostfs / doorbell
  2. First-time OS build via ``common.sh`` / ``quick-build.sh`` (unless ``--no-build``)
  3. For each selected experiment: ``timeout … artifact-evaluation/<dir>/run.sh``
  4. Then plot / parse figures via each directory's ``plot.py`` (or ``plot.sh``)

Each experiment owns build, QEMU, and output paths. ``run_all.py`` always
attempts plotting after ``run.sh`` so partial logs still produce figures when
a run times out or ``run.sh`` exits before its own plot step. Use
``--plot-only`` to re-plot from existing logs without re-running QEMU.

Examples::

    ./artifact-evaluation/run_all.py --clean
    ./artifact-evaluation/run_all.py --run-subset-of-tests 1,4,7
    ./artifact-evaluation/run_all.py --no-prepare --no-build --run-subset-of-tests 1
    ./artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 3
    ./artifact-evaluation/run_all.py --dry-run --clean --run-subset-of-tests 1,4
    ./artifact-evaluation/run_all.py --list
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


AE_ROOT = Path(__file__).resolve().parent
REPO_ROOT = AE_ROOT.parent

# Paper / README numbering (0 = extra basic setup).
EXPERIMENT_NUMBERS: Dict[int, str] = {
    0: "basic",
    1: "ipc-cdf",
    2: "sched-notify",
    3: "memory-allocator",
    4: "state-partition",
    5: "auto-scale",
    6: "resource-util",
    7: "recover-fs",
    8: "dbx1000-cross-warehouse",
    9: "queue-saturation",
}

PAPER_ORDER = [
    "ipc-cdf",
    "memory-allocator",
    "state-partition",
    "auto-scale",
    "resource-util",
    "recover-fs",
]
# Default one-click set: only the experiments that produce a paper figure,
# i.e. the original submission figures plus the camera-ready revision-plan
# additions (8-machine cross-warehouse sweep, per-service-queue saturation).
# Excludes 0-basic (Table 3 setup check) and 2-sched-notify (Section 8.2 text,
# no figure) — run those explicitly via --run-subset-of-tests if needed.
#
# auto-scale runs LAST on purpose.  It is the only experiment that starts the
# Tigon baseline, which builds an mkosi image and brings up an eight-VM CXL-pod
# environment, changing host VM, network, mount and CPU state.  If that stage
# wedges the host, every other figure has already been produced and plotted.
READY_PAPER = [
    "ipc-cdf",
    "queue-saturation",
    "memory-allocator",
    "state-partition",
    "resource-util",
    "recover-fs",
    "dbx1000-cross-warehouse",
    "auto-scale",
]
EXTRA_ORDER = [
    "basic",
    "sched-notify",
]

LEGACY_OUTPUT_DIRS = ("logs", "csv", "figures")

AE_SESSIONS = [
    "{user}-ae",
    "{user}-ipc-ae",
    "{user}-sched-notify-ae",
    "{user}-recover-fs-ae",
    "{user}-msi-basic-ae",
    "{user}-qemu",
]


@dataclass(frozen=True)
class Experiment:
    name: str
    directory: str
    status: str  # ready | stub
    budget_s: int
    paper_fig: str
    is_paper: bool = True
    # Wall-clock ceiling under the default fast profile (see FAST_ENV).  None
    # means the fast profile does not shrink this experiment.
    fast_budget_s: Optional[int] = None


EXPERIMENTS: Dict[str, Experiment] = {
    "basic": Experiment(
        "basic", "0-basic", "ready", 3600,
        "setup Table 1 (MLC) + MSI transport", is_paper=False,
    ),
    "ipc-cdf": Experiment(
        "ipc-cdf", "1-ipc-cdf", "ready", 10800,
        "IPC CDF + breakdown",
    ),
    "sched-notify": Experiment(
        "sched-notify", "2-sched-notify-latency", "ready", 3600,
        "(extra) sched/notify + Linux host baseline", is_paper=False,
    ),
    "memory-allocator": Experiment(
        "memory-allocator", "3-memory-allocator", "ready", 28800,
        "allocator fig00", fast_budget_s=18000,
    ),
    "state-partition": Experiment(
        # Camera-ready measures only the 8-machine panel: three shared
        # placements at 8 x 12 vCPUs plus the single-machine Private baseline,
        # i.e. 4 builds x 6 benchmarks.  run.sh owns that default
        # (MACHINE_COUNTS), so there is no fast/full split here.  Measured at
        # 0.85 h for this scope and 1.6 h for the wider MACHINE_COUNTS="4 8"
        # sweep; 4 h leaves room for both plus the host's ~3x contention tail.
        "state-partition", "4-state-partition", "ready", 14400,
        "state_partition",
    ),
    "auto-scale": Experiment(
        "auto-scale", "5-auto-scale", "ready", 64800,
        "auto-scale-matrix / db1000 / gemini", fast_budget_s=50400,
    ),
    "resource-util": Experiment(
        "resource-util", "6-resource-util", "ready", 43200,
        "real.png",
    ),
    "recover-fs": Experiment(
        "recover-fs", "7-recover-fs", "ready", 10800,
        "recovery-performance-single",
    ),
    "dbx1000-cross-warehouse": Experiment(
        "dbx1000-cross-warehouse", "8-dbx1000-cross-warehouse", "ready", 21600,
        "(camera-ready) cross-warehouse sweep", fast_budget_s=10800,
    ),
    "queue-saturation": Experiment(
        "queue-saturation", "9-queue-saturation", "ready", 10800,
        "(camera-ready) queue tail latency + saturation", fast_budget_s=9000,
    ),
}

# Fast profile (the default; disable with --full).  Two levers only:
#   1. one measurement per point instead of three, where a repeat knob exists;
#   2. a thinned sweep axis, dropping points adjacent to ones that are kept.
# Every value here is a documented scope control of the experiment's own
# run.sh, so each script still validates it and switches its plotter to
# --allow-partial by itself.  Figures keep every series; they lose error bars
# and some x resolution.
#
# state-partition needs no entry here: Figure 13 is the 8-machine panel only,
# so its run.sh already defaults to MACHINE_COUNTS="8" under --full as well.
#
# NOTE: dbx1000/All_DRAM/8 is currently broken and is deliberately NOT listed
# in SKIP_POINTS.  A Private guest gets one 16 GiB DRAM device regardless of
# DBX_DRAM_SIZE (kernel DRAM comes from dsm-scripts/numa_sizes.conf, not from
# QEMU's -m), so the 8-machine panel's 64 warehouses OOM during the TPC-C load
# and then fail the plot step via --require-point.  The point aborts early
# rather than burning DBX_TIMEOUT: its "BUG: handle_trans_fault" matches
# AE_ERROR_PATTERN, so ae_wait_in_log stops it in ~208 s (measured).
# Masking that with SKIP_POINTS would silently drop DBx1000 from Figure 13,
# because it also removes the Private baseline every other placement is
# normalized against.  The OOM is being fixed properly instead; until then this
# point is expected to fail loudly.  Set SKIP_POINTS=dbx1000/All_DRAM/8 in the
# environment to opt back into skipping it.
FAST_ENV: Dict[str, Dict[str, str]] = {
    "memory-allocator": {"USER_BENCH_THREADS": "1 4 16 64 96"},
    # The tigon baseline stays in: measured runtimes show the whole artifact
    # fits comfortably in a day, so Figure 14 keeps its Tigon curve.  It is the
    # slowest and most host-invasive stage (mkosi image build, eight-VM CXL-pod
    # environment, host VM/network/mount/CPU state), so it is scheduled last
    # twice over: run_baselines.py always runs linux -> matrix-tcp -> tigon,
    # and auto-scale is the final entry in READY_PAPER.
    "auto-scale": {"MACHINES": "1 2 4 8", "RUN_FOOTPRINT": "0"},
    "dbx1000-cross-warehouse": {"DBX_REPETITIONS": "1"},
    # queue-saturation keeps REPEATS=3: its plot.py drops the lowest and
    # highest trial per point, and below three trials there is nothing left to
    # trim, so it falls back to a plain mean.  That still plots, but it is not
    # what the paper reports -- thin the thread axis here instead.
    "queue-saturation": {"THREADS": "1 2 4 8 10"},
}


def log(msg: str = "") -> None:
    print(msg, flush=True)


def exp_dir(name: str) -> Path:
    return AE_ROOT / EXPERIMENTS[name].directory


def run_script(name: str) -> Path:
    """Each experiment's entry point (always run.sh under its directory)."""
    return exp_dir(name) / "run.sh"


def latest_out_dir(ae_dir: Path) -> Optional[Path]:
    """Newest artifact-evaluation/<dir>/out/<timestamp> from a prior sweep."""
    out_root = ae_dir / "out"
    if not out_root.is_dir():
        return None
    candidates = [p for p in out_root.iterdir() if p.is_dir()]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def ae_output_dirs(out_dir: Path) -> tuple[Path, Path, Path]:
    """Per-run artifact layout under out/<timestamp>/."""
    return out_dir / "logs", out_dir / "csv", out_dir / "figures"


def experiment_number(name: str) -> Optional[int]:
    for num, exp_name in EXPERIMENT_NUMBERS.items():
        if exp_name == name:
            return num
    return None


def parse_subset(raw: str) -> List[str]:
    """Parse comma-separated experiment numbers (spaces trimmed)."""
    numbers: List[int] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            numbers.append(int(part))
        except ValueError as exc:
            raise SystemExit(
                f"Invalid experiment number: {part!r} (expected comma-separated integers)"
            ) from exc

    if not numbers:
        raise SystemExit("--run-subset-of-tests requires at least one experiment number")

    unknown = [n for n in numbers if n not in EXPERIMENT_NUMBERS]
    if unknown:
        valid = ", ".join(str(n) for n in sorted(EXPERIMENT_NUMBERS))
        bad = ", ".join(str(n) for n in unknown)
        raise SystemExit(f"Unknown experiment number(s): {bad} (valid: {valid})")

    return [EXPERIMENT_NUMBERS[n] for n in numbers]


def resolve_experiments(subset: Optional[str]) -> List[str]:
    if subset is not None:
        return parse_subset(subset)
    return list(READY_PAPER)


def clean_targets() -> List[Path]:
    """Paths removed by --clean: out/ and legacy flat output dirs per experiment."""
    targets: List[Path] = []
    seen: set[Path] = set()
    for exp in EXPERIMENTS.values():
        ae_dir = AE_ROOT / exp.directory
        for rel in ("out", *LEGACY_OUTPUT_DIRS):
            path = ae_dir / rel
            if path.exists() and path not in seen:
                targets.append(path)
                seen.add(path)
    return sorted(targets)


def clean_ae_outputs(*, dry_run: bool) -> None:
    targets = clean_targets()
    if not targets:
        log("=== --clean: no AE output directories to remove ===")
        return

    log("=== --clean: removing AE output directories ===")
    for path in targets:
        rel = path.relative_to(AE_ROOT)
        if dry_run:
            log(f"[dry-run] would remove: artifact-evaluation/{rel}")
            continue
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink()
        log(f"removed: artifact-evaluation/{rel}")


def plot_cmd(name: str) -> Optional[List[str]]:
    """Build the plot/parse command for an experiment (None if no plotter)."""
    ae_dir = exp_dir(name)
    out = latest_out_dir(ae_dir)
    plot_sh = ae_dir / "plot.sh"
    if plot_sh.is_file():
        return ["bash", str(plot_sh)]

    plot_py = ae_dir / "plot.py"
    parse_msi = ae_dir / "parse_msi.py"

    if name == "basic":
        if not parse_msi.is_file():
            return None
        if out is None:
            return ["python3", str(parse_msi)]
        log_dir, csv_dir, _ = ae_output_dirs(out)
        return [
            "python3", str(parse_msi),
            "--log-dir", str(log_dir),
            "--csv-dir", str(csv_dir),
        ]

    if not plot_py.is_file():
        return None

    user = os.environ.get("USER", "user")
    mplconfig = os.environ.get("MPLCONFIGDIR", f"/tmp/matplotlib-{user}")
    if name in ("memory-allocator", "recover-fs"):
        base = ["env", f"MPLCONFIGDIR={mplconfig}", "python3", str(plot_py)]
    else:
        base = ["python3", str(plot_py)]

    if out is None:
        if name in ("ipc-cdf", "queue-saturation", "sched-notify",
                    "memory-allocator", "recover-fs"):
            return base
        return None

    log_dir, csv_dir, fig_dir = ae_output_dirs(out)

    if name == "memory-allocator":
        return base + [
            "--csv", str(csv_dir / "allocator_results.csv"),
            "--fig-dir", str(fig_dir),
        ]

    if name == "recover-fs":
        return base + [
            "--detail", str(csv_dir / "recovery_detail.csv"),
            "--throughput", str(csv_dir / "throughput.csv"),
            "--fig-dir", str(fig_dir),
        ]

    if name in ("ipc-cdf", "queue-saturation", "sched-notify"):
        return base + [
            "--log-dir", str(log_dir),
            "--csv-dir", str(csv_dir),
            "--fig-dir", str(fig_dir),
        ]

    if name == "auto-scale":
        cmd = base + ["--csv-dir", str(csv_dir), "--fig-dir", str(fig_dir)]
        if log_dir.is_dir() and any(log_dir.glob("*_N*.log")):
            return cmd + ["--log-dir", str(log_dir)]
        for flag, fname in (
            ("--matrix-data", "4000size.txt"),
            ("--db1000-data", "db1000-p3os-tigon.csv"),
            ("--gemini-data", "gemini-data.log"),
        ):
            path = csv_dir / fname
            if path.is_file():
                cmd += [flag, str(path)]
        return cmd if len(cmd) > 4 else None

    if name in ("state-partition", "resource-util", "dbx1000-cross-warehouse"):
        return base + [
            "--log-dir", str(log_dir),
            "--csv-dir", str(csv_dir),
            "--fig-dir", str(fig_dir),
        ]

    return base


def budget_for(exp: Experiment, global_budget: Optional[int], *,
               fast: bool = True) -> int:
    key = f"BUDGET_{exp.name.upper().replace('-', '_')}"
    raw = os.environ.get(key)
    if raw:
        return int(raw)
    if global_budget is not None:
        return global_budget
    if fast and exp.fast_budget_s is not None:
        return exp.fast_budget_s
    return exp.budget_s


def fast_env_for(name: str) -> Dict[str, str]:
    """Fast-profile scope overrides for one experiment.

    An override already present in the caller's environment wins, so an
    evaluator can widen a single axis back without leaving the fast profile.
    """
    return {
        var: value
        for var, value in FAST_ENV.get(name, {}).items()
        if var not in os.environ
    }


def kill_ae_sessions() -> None:
    subprocess.run(
        [
            "bash", "-c",
            f'source "{AE_ROOT}/common.sh" && ae_ensure_clean_tmux',
        ],
        cwd=str(REPO_ROOT),
        check=False,
    )


def abort_ae_sessions() -> None:
    """Immediately tear down AE guests and release abandoned runner locks.

    The normal cleanup path gives QEMU time to exit gracefully, which is
    useful between successful experiments but makes Ctrl-C appear ignored.
    This path intentionally skips those waits.
    """
    env = os.environ.copy()
    env["AE_FORCE_STOP_SKIP_PID"] = str(os.getpid())
    subprocess.run(
        [
            "bash", "-c",
            f'source "{AE_ROOT}/common.sh" && '
            "ae_force_stop_artifact_runners",
        ],
        cwd=str(REPO_ROOT),
        env=env,
        check=False,
    )


def interrupt_handler(_signum: int, _frame: object) -> None:
    """Route both Ctrl-C and `kill <run_all_pid>` through one cleanup path."""
    raise KeyboardInterrupt


def run_cmd(
    cmd: List[str],
    *,
    cwd: Path = REPO_ROOT,
    env: Optional[Dict[str, str]] = None,
) -> int:
    """Run one stage in its own process group, with responsive Ctrl-C."""
    process = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        env=env,
        start_new_session=True,
    )
    try:
        return process.wait()
    except KeyboardInterrupt:
        log("[run-all] interrupt received; stopping current command...")
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            log("[run-all] current command did not exit in 3s; sending SIGKILL")
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        raise


@dataclass
class BuildConfigSnapshot:
    """A stable configuration baseline for an entire multi-test AE run.

    Individual experiments deliberately change these files while selecting a
    workload.  Restoring this snapshot before every experiment prevents an
    interrupted experiment (or an incomplete local cleanup path) from
    affecting the vCPU/build configuration of the next one.
    """

    directory: Path

    FILES = (
        Path(".config"),
        Path("chcore.ini"),
        Path("kernel/dsm_config.cmake"),
        Path("user/demos/config.cmake"),
    )

    @classmethod
    def create(cls) -> "BuildConfigSnapshot":
        directory = Path(tempfile.mkdtemp(prefix="starfishos-ae-config-"))
        snapshot = cls(directory)
        for relative in snapshot.FILES:
            source = REPO_ROOT / relative
            if not source.is_file():
                snapshot.remove()
                raise RuntimeError(f"cannot snapshot missing AE config: {source}")
            destination = directory / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        log(f"[run-all] saved per-test config baseline: {directory}")
        return snapshot

    def restore(self) -> None:
        for relative in self.FILES:
            source = self.directory / relative
            destination = REPO_ROOT / relative
            if not source.is_file():
                raise RuntimeError(f"AE config snapshot is incomplete: {source}")
            shutil.copy2(source, destination)
        log("[run-all] restored config baseline before experiment")

    def set_guest_cpu_num(self, cpu_num: int) -> None:
        """Set both the compile-time CPU stride and QEMU default for one test."""
        replacements = (
            (Path(".config"), r"^CHCORE_PLAT_CPU_NUM:STRING=.*$",
             f"CHCORE_PLAT_CPU_NUM:STRING={cpu_num}"),
            (Path("chcore.ini"), r"^cpu_num\s*=.*$", f"cpu_num = {cpu_num}"),
        )
        for relative, pattern, replacement in replacements:
            path = REPO_ROOT / relative
            updated, count = re.subn(pattern, replacement, path.read_text(), flags=re.MULTILINE)
            if count != 1:
                raise RuntimeError(f"failed to set CPU profile in {path}")
            path.write_text(updated)
        log(f"[run-all] configured this experiment for {cpu_num} vCPUs per QEMU")

    def disable_llfree(self) -> None:
        """Start every experiment from the non-LLFree allocator baseline."""
        path = REPO_ROOT / "kernel/dsm_config.cmake"
        updated, count = re.subn(
            r'^set\(DSM_CXL_LF_BUDDY\s+"[A-Z]+"\)$',
            'set(DSM_CXL_LF_BUDDY "OFF")',
            path.read_text(),
            flags=re.MULTILINE,
        )
        if count != 1:
            raise RuntimeError(f"failed to disable LLFree in {path}")
        path.write_text(updated)

    def remove(self) -> None:
        shutil.rmtree(self.directory, ignore_errors=True)


def needs_graph_dataset(names: List[str]) -> bool:
    """Only auto-scale currently consumes the ~11 GiB Gemini graph."""
    return "auto-scale" in names


def guest_cpu_num(name: str) -> int:
    """AE CPU profile: allocator is 96 vCPU; every other test uses 12."""
    return 96 if name == "memory-allocator" else 12


def ensure_prepare(*, skip: bool, include_graph: bool,
                   include_paper_deps: bool) -> None:
    if skip:
        log("=== Skipping prepare.sh (--no-prepare) ===")
        return
    log("=== Global preparation (prepare.sh ensure) ===")
    env = os.environ.copy()
    if "SKIP_GRAPH_DATASET" not in env and not include_graph:
        env["SKIP_GRAPH_DATASET"] = "1"
        log("=== Ready/nongraph mode: skipping twitter-2010.bin (~11 GiB) ===")
    if include_paper_deps:
        env["AE_INCLUDE_OPTIONAL_PAPER"] = "1"
    rc = subprocess.run(
        [str(AE_ROOT / "prepare.sh"), "ensure"],
        cwd=str(REPO_ROOT),
        env=env,
        check=False,
    ).returncode
    if rc != 0:
        raise SystemExit("prepare.sh ensure failed (rc={})".format(rc))


def ensure_base_build(*, skip: bool) -> None:
    if skip:
        log("=== Skipping first-time OS build (--no-build) ===")
        return
    log("=== Ensuring first-time OS build (if needed) ===")
    env = os.environ.copy()
    if env_flag("FORCE_BASE_BUILD"):
        env["FORCE_BASE_BUILD"] = "1"
    rc = subprocess.run(
        [
            "bash", "-c",
            f'source "{AE_ROOT}/common.sh" && ae_ensure_base_build',
        ],
        cwd=str(REPO_ROOT),
        env=env,
        check=False,
    ).returncode
    if rc != 0:
        raise SystemExit("First-time OS build check failed")


def ensure_runtime_resources() -> bool:
    """Validate persistent backing files and self-heal the doorbell server."""
    return subprocess.run(
        [
            "bash", "-c",
            f'source "{AE_ROOT}/common.sh" && ae_check_global_prepare',
        ],
        cwd=str(REPO_ROOT),
        check=False,
    ).returncode == 0


def run_experiment(
    name: str,
    *,
    budget: int,
    dry_run: bool,
    fast: bool = True,
    config_snapshot: Optional[BuildConfigSnapshot] = None,
) -> str:
    exp = EXPERIMENTS[name]
    script = run_script(name)
    ae_dir = exp_dir(name)

    log("")
    log("#" * 60)
    log(f"### [{time.strftime('%H:%M:%S')}] {name}")
    log(f"###   dir={exp.directory}  status={exp.status}  budget={budget}s")
    log(f"###   script={script}")
    log(f"###   output={ae_dir}")
    overrides = fast_env_for(name) if fast else {}
    if overrides:
        rendered = " ".join(f"{k}={v!r}" for k, v in sorted(overrides.items()))
        log(f"###   fast profile: {rendered}")
    log("#" * 60)

    if exp.status == "stub":
        log(f"[run-all] SKIP stub experiment (no run.sh yet): {name}")
        return "TODO(stub)"

    if not script.is_file():
        log(f"[run-all] MISSING {script}")
        return f"MISSING({script.name})"

    if dry_run:
        log(
            f"[dry-run] would configure {guest_cpu_num(name)} vCPUs per QEMU "
            "for this experiment"
        )
        log(f"[dry-run] would run: timeout --kill-after=60 {budget} {script}")
        return "DRY_RUN"

    if config_snapshot is not None:
        try:
            config_snapshot.restore()
            config_snapshot.set_guest_cpu_num(guest_cpu_num(name))
            config_snapshot.disable_llfree()
        except RuntimeError as exc:
            log(f"[run-all] failed to restore config before {name}: {exc}")
            return "CONFIG_RESTORE_FAILED"

    if not ensure_runtime_resources():
        log(f"[run-all] global AE resources unavailable before {name}")
        return "PREPARE_FAILED"

    kill_ae_sessions()
    env = os.environ.copy()
    env["CPU_NUM"] = str(guest_cpu_num(name))
    env.update(overrides)
    rc = run_cmd(
        ["timeout", "--kill-after=60", str(budget), str(script)], env=env,
    )
    if rc == 0:
        return "OK"
    if rc in (124, 137):
        kill_ae_sessions()
        return f"TIMEOUT({budget}s)"
    kill_ae_sessions()
    return f"FAILED(rc={rc})"


def run_plot(name: str, *, dry_run: bool) -> str:
    exp = EXPERIMENTS[name]
    ae_dir = exp_dir(name)
    cmd = plot_cmd(name)

    log("")
    log(f"--- [{time.strftime('%H:%M:%S')}] plot {name} ---")

    if exp.status == "stub":
        log(f"[plot] SKIP stub experiment: {name}")
        return "TODO(stub)"

    if cmd is None:
        log(f"[plot] no plotter or no inputs for {name} (skip)")
        return "NO_INPUT"

    log(f"[plot] cmd: {' '.join(cmd)}")

    if dry_run:
        return "DRY_RUN"

    rc = run_cmd(cmd)
    if rc == 0:
        log(f"[plot] OK -> {ae_dir}")
        return "OK"
    log(f"[plot] FAILED(rc={rc}) for {name}")
    return f"FAILED(rc={rc})"


def env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run artifact-evaluation experiments (each directory's run.sh).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Remove artifact-evaluation/*/out/ and legacy flat logs/csv/figures; "
             "alone exits after clean, combined with a run cleans first",
    )
    parser.add_argument(
        "--run-subset-of-tests",
        metavar="N[,N...]",
        help="Run only numbered experiments (see --list); default is the ready set",
    )
    parser.add_argument(
        "--plot-only",
        action="store_true",
        help="Re-plot from the latest out/<timestamp>/ without re-running QEMU",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=env_flag("DRY_RUN"),
        help="Print actions without running prepare, build, experiments, or clean",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List experiments with paper numbers and exit",
    )
    parser.add_argument(
        "--no-prepare",
        action="store_true",
        help="Skip artifact-evaluation/prepare.sh",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Skip first-time OS build check",
    )
    parser.add_argument(
        "--budget",
        type=int,
        default=None,
        help="Override wall-clock timeout (seconds) for each run.sh",
    )
    parser.add_argument(
        "--full",
        dest="fast",
        action="store_false",
        default=not env_flag("AE_FULL"),
        help="Disable the default fast profile and run every sweep point with "
             "the paper's repetition counts (much longer; see --list)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    if args.list:
        log("Experiments (each uses artifact-evaluation/<dir>/run.sh):")
        for num in sorted(EXPERIMENT_NUMBERS):
            name = EXPERIMENT_NUMBERS[num]
            exp = EXPERIMENTS[name]
            tag = "paper" if exp.is_paper else "extra"
            script = run_script(name)
            present = "run.sh" if script.is_file() else "MISSING run.sh"
            fast_budget = budget_for(exp, None, fast=True)
            note = ""
            if fast_budget != exp.budget_s:
                note = (f"  fast<={fast_budget // 3600}h "
                        f"(full {exp.budget_s // 3600}h)")
            elif name in FAST_ENV:
                note = f"  fast<={fast_budget // 3600}h"
            log(
                f"  {num}  {name:<26} {exp.status:<6} [{tag}]  "
                f"{exp.directory}/{present}{note}"
            )
        log("")
        log(f"Default run set (no --run-subset-of-tests): {' '.join(READY_PAPER)}")
        log("Default profile: fast (pass --full for the paper's full sweeps)")
        for name in READY_PAPER:
            overrides = FAST_ENV.get(name)
            if overrides:
                rendered = " ".join(
                    f"{k}={v!r}" for k, v in sorted(overrides.items()))
                log(f"  fast {name:<26} {rendered}")
        return 0

    names = resolve_experiments(args.run_subset_of_tests)
    clean_only = args.clean and not args.plot_only and args.run_subset_of_tests is None

    do_prepare = not args.no_prepare and not args.plot_only
    do_build = not args.no_build and not args.plot_only
    do_run = not args.plot_only

    log("=== One-click AE: artifact-evaluation/run_all.py ===")
    if args.run_subset_of_tests is not None:
        log(f"=== subset: {args.run_subset_of_tests} ===")
    else:
        log("=== subset: (default ready set) ===")
    log(f"=== experiments: {' '.join(names)} ===")
    log(f"=== profile: {'fast' if args.fast else 'full (paper sweeps)'} ===")
    log(
        f"=== stages: prepare={do_prepare} build={do_build} "
        f"run={do_run} plot=True dry_run={args.dry_run} clean={args.clean} ==="
    )

    if args.clean:
        clean_ae_outputs(dry_run=args.dry_run)
        if clean_only:
            return 0

    if args.plot_only and not args.dry_run:
        log("=== plot-only: re-plot from existing logs (no QEMU) ===")

    if args.dry_run:
        if do_prepare:
            log("[dry-run] would run: prepare.sh ensure")
        if do_build:
            log("[dry-run] would run: ae_ensure_base_build (common.sh)")

    if not args.dry_run:
        if do_prepare:
            ensure_prepare(
                skip=False,
                include_graph=needs_graph_dataset(names),
                include_paper_deps=bool({"auto-scale", "resource-util"} & set(names)),
            )

        if do_build:
            ensure_base_build(skip=False)

    status: Dict[str, str] = {}
    plot_status: Dict[str, str] = {}
    overall = 0
    config_snapshot: Optional[BuildConfigSnapshot] = None

    if do_run and not args.dry_run:
        try:
            config_snapshot = BuildConfigSnapshot.create()
        except RuntimeError as exc:
            log(f"[run-all] unable to create per-test config baseline: {exc}")
            return 1

    try:
        if do_run:
            for name in names:
                exp = EXPERIMENTS[name]
                status[name] = run_experiment(
                    name,
                    budget=budget_for(exp, args.budget, fast=args.fast),
                    dry_run=args.dry_run,
                    fast=args.fast,
                    config_snapshot=config_snapshot,
                )
                st = status[name]
                if st not in ("OK", "DRY_RUN", "TODO(stub)") and not st.startswith("TODO"):
                    overall = 1
                plot_status[name] = run_plot(name, dry_run=args.dry_run)
                pst = plot_status[name]
                if pst not in ("OK", "DRY_RUN", "TODO(stub)", "NO_INPUT"):
                    overall = 1
        else:
            for name in names:
                plot_status[name] = run_plot(name, dry_run=args.dry_run)
                pst = plot_status[name]
                if pst not in ("OK", "DRY_RUN", "TODO(stub)", "NO_INPUT"):
                    overall = 1
    finally:
        if config_snapshot is not None:
            try:
                config_snapshot.restore()
                log("[run-all] restored config baseline after all experiments")
            except RuntimeError as exc:
                log(f"[run-all] failed to restore final config baseline: {exc}")
                overall = 1
            config_snapshot.remove()

    log("")
    log("#" * 60)
    log("### Summary")
    log("#" * 60)
    ready_ok = ready_total = 0
    for name in names:
        st = status.get(name, "skipped")
        pst = plot_status.get(name, "-")
        num = experiment_number(name)
        prefix = f"{num} " if num is not None else "  "
        log(f"{prefix}{name:<26} run={st:<20} plot={pst:<16} {exp_dir(name)}")
        if st not in ("DRY_RUN", "TODO(stub)", "skipped"):
            ready_total += 1
            if st == "OK":
                ready_ok += 1

    log(f"Ready experiments OK: {ready_ok} / {ready_total}")
    return overall


if __name__ == "__main__":
    signal.signal(signal.SIGINT, interrupt_handler)
    signal.signal(signal.SIGTERM, interrupt_handler)
    signal.signal(signal.SIGHUP, interrupt_handler)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        # Do not let a second Ctrl-C / TERM interrupt the forced cleanup.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        log("\n[run-all] interrupted; forcibly stopping AE tmux/QEMU state.")
        abort_ae_sessions()
        sys.exit(130)
