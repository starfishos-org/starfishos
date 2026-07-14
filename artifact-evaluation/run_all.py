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

    ./artifact-evaluation/run_all.py --prepare-only
    ./artifact-evaluation/run_all.py --build-only --force-base-build
    ./artifact-evaluation/run_all.py --no-prepare --no-build ipc-cdf
    ./artifact-evaluation/run_all.py --experiments-only
    ./artifact-evaluation/run_all.py paper          # full paper set (includes stubs / unvalidated)
    ./artifact-evaluation/run_all.py --dry-run
    ./artifact-evaluation/run_all.py --list
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


AE_ROOT = Path(__file__).resolve().parent
REPO_ROOT = AE_ROOT.parent

PAPER_ORDER = [
    "ipc-cdf",
    "memory-allocator",
    "state-partition",
    "auto-scale",
    "process-migration",
    "resource-util",
    "recover-fs",
]
READY_PAPER = [
    "ipc-cdf",
    "memory-allocator",
    "state-partition",
    "recover-fs",
]
EXTRA_ORDER = [
    "basic",
    "sched-notify",
    "dbx1000-cross-warehouse",
]

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
        "allocator fig00",
    ),
    "state-partition": Experiment(
        "state-partition", "4-state-partition", "ready", 36000,
        "state_partition",
    ),
    "auto-scale": Experiment(
        "auto-scale", "5-auto-scale", "ready", 28800,
        "auto-scale-matrix / db1000 / gemini",
    ),
    "resource-util": Experiment(
        "resource-util", "6-resource-util", "ready", 21600,
        "real.eps",
    ),
    "recover-fs": Experiment(
        "recover-fs", "7-recover-fs", "ready", 10800,
        "recovery-performance-single",
    ),
    "process-migration": Experiment(
        "process-migration", "8-process-migration", "stub", 14400,
        "process-migration",
    ),
    "dbx1000-cross-warehouse": Experiment(
        "dbx1000-cross-warehouse", "8-dbx1000-cross-warehouse", "ready", 21600,
        "(extra) cross-warehouse", is_paper=False,
    ),
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


def plot_cmd(name: str) -> Optional[List[str]]:
    """Build the plot/parse command for an experiment (None if no plotter)."""
    ae_dir = exp_dir(name)
    plot_sh = ae_dir / "plot.sh"
    if plot_sh.is_file():
        return ["bash", str(plot_sh)]

    plot_py = ae_dir / "plot.py"
    parse_msi = ae_dir / "parse_msi.py"

    if name == "basic":
        return ["python3", str(parse_msi)] if parse_msi.is_file() else None
    if not plot_py.is_file():
        return None

    user = os.environ.get("USER", "user")
    mplconfig = os.environ.get("MPLCONFIGDIR", f"/tmp/matplotlib-{user}")
    if name in ("memory-allocator", "recover-fs"):
        base = ["env", f"MPLCONFIGDIR={mplconfig}", "python3", str(plot_py)]
    else:
        base = ["python3", str(plot_py)]

    if name in ("ipc-cdf", "sched-notify", "memory-allocator", "recover-fs"):
        return base

    out = latest_out_dir(ae_dir)
    if out is None:
        return None

    log_dir = out / "logs"
    if name == "auto-scale":
        results = out / "results"
        cmd = base + ["--out-dir", str(out)]
        for flag, fname in (
            ("--matrix-data", "4000size.txt"),
            ("--db1000-data", "db1000-p3os-tigon.csv"),
            ("--gemini-data", "gemini-data.log"),
        ):
            path = results / fname
            if path.is_file():
                cmd += [flag, str(path)]
        return cmd if len(cmd) > 4 else None

    if name in ("state-partition", "resource-util", "dbx1000-cross-warehouse"):
        return base + ["--log-dir", str(log_dir), "--out-dir", str(out)]

    return base


def budget_for(exp: Experiment, global_budget: Optional[int]) -> int:
    key = f"BUDGET_{exp.name.upper().replace('-', '_')}"
    raw = os.environ.get(key)
    if raw:
        return int(raw)
    if global_budget is not None:
        return global_budget
    return exp.budget_s


def resolve_names(mode_args: List[str]) -> List[str]:
    # Default is the validated ready set so a fresh-clone one-click path can succeed.
    if not mode_args or mode_args == ["ready"]:
        return list(READY_PAPER)
    head = mode_args[0]
    if head == "paper":
        return list(PAPER_ORDER)
    if head == "ready":
        return list(READY_PAPER)
    if head == "extra":
        return list(EXTRA_ORDER)
    if head == "all":
        return list(PAPER_ORDER) + list(EXTRA_ORDER)
    if head in ("-h", "--help", "help"):
        return []
    unknown = [n for n in mode_args if n not in EXPERIMENTS]
    if unknown:
        raise SystemExit(f"Unknown experiment(s): {', '.join(unknown)}")
    return list(mode_args)


def kill_ae_sessions() -> None:
    subprocess.run(
        [
            "bash", "-c",
            f'source "{AE_ROOT}/common.sh" && ae_ensure_clean_tmux',
        ],
        cwd=str(REPO_ROOT),
        check=False,
    )


def run_cmd(cmd: List[str], *, cwd: Path = REPO_ROOT) -> int:
    return subprocess.run(cmd, cwd=str(cwd), check=False).returncode


def ensure_prepare(*, skip: bool, mode: str) -> None:
    if skip:
        log("=== Skipping prepare.sh (--no-prepare) ===")
        return
    log(f"=== Global preparation (prepare.sh {mode}) ===")
    rc = run_cmd([str(AE_ROOT / "prepare.sh"), mode])
    if rc != 0:
        raise SystemExit(f"prepare.sh {mode} failed (rc={rc})")


def ensure_base_build(*, skip: bool, force: bool) -> None:
    if skip:
        log("=== Skipping first-time OS build (--no-build) ===")
        return
    log("=== Ensuring first-time OS build (if needed) ===")
    env = os.environ.copy()
    if force:
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


def run_experiment(
    name: str,
    *,
    budget: int,
    dry_run: bool,
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
    log("#" * 60)

    if exp.status == "stub":
        log(f"[run-all] SKIP stub experiment (no run.sh yet): {name}")
        return "TODO(stub)"

    if not script.is_file():
        log(f"[run-all] MISSING {script}")
        return f"MISSING({script.name})"

    if dry_run:
        log(f"[dry-run] would run: timeout --kill-after=60 {budget} {script}")
        return "DRY_RUN"

    kill_ae_sessions()
    rc = run_cmd(
        ["timeout", "--kill-after=60", str(budget), str(script)],
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
        "mode",
        nargs="*",
        default=["ready"],
        help="ready (default) | paper | all | extra | experiment names",
    )

    stages = parser.add_argument_group("stage shortcuts")
    stages.add_argument("--prepare-only", action="store_true")
    stages.add_argument("--build-only", action="store_true")
    stages.add_argument(
        "--plot-only",
        action="store_true",
        help="Re-plot from existing logs; skip prepare, build, and run.sh",
    )
    stages.add_argument(
        "--gather-only",
        action="store_true",
        help="Alias for --plot-only",
    )

    parser.add_argument(
        "--plot", dest="do_plot", action="store_true", default=None,
    )
    parser.add_argument(
        "--no-plot", dest="do_plot", action="store_false",
    )
    stages.add_argument(
        "--experiments-only",
        action="store_true",
        help="Skip prepare + OS build; only run experiments",
    )

    parser.add_argument(
        "--prepare", dest="do_prepare", action="store_true", default=None,
    )
    parser.add_argument(
        "--no-prepare", "--skip-prepare",
        dest="do_prepare", action="store_false",
    )
    parser.add_argument(
        "--prepare-mode",
        choices=("ensure", "recreate"),
        default=os.environ.get("PREPARE_MODE", "ensure"),
    )
    parser.add_argument(
        "--build", dest="do_build", action="store_true", default=None,
    )
    parser.add_argument(
        "--no-build", "--skip-base-build",
        dest="do_build", action="store_false",
    )
    parser.add_argument(
        "--force-base-build",
        action="store_true",
        default=env_flag("FORCE_BASE_BUILD"),
    )
    parser.add_argument(
        "--run", dest="do_run", action="store_true", default=None,
    )
    parser.add_argument(
        "--no-run", dest="do_run", action="store_false",
    )
    parser.add_argument(
        "--budget", type=int, default=None,
        help="Override wall-clock timeout (seconds) for each run.sh",
    )
    parser.add_argument(
        "--dry-run", action="store_true", default=env_flag("DRY_RUN"),
    )
    parser.add_argument("--list", action="store_true")

    args = parser.parse_args(argv)

    if args.prepare_only:
        args.do_prepare, args.do_build, args.do_run = True, False, False
    elif args.build_only:
        args.do_prepare = False if args.do_prepare is None else args.do_prepare
        args.do_build, args.do_run = True, False
    elif args.plot_only or args.gather_only:
        args.do_prepare, args.do_build, args.do_run = False, False, False
        args.do_plot = True
    elif args.experiments_only:
        args.do_prepare, args.do_build = False, False
        if args.do_run is None:
            args.do_run = True
    else:
        if args.do_prepare is None:
            args.do_prepare = not env_flag("SKIP_PREPARE")
        if args.do_build is None:
            args.do_build = not env_flag("SKIP_BASE_BUILD")
        if args.do_run is None:
            args.do_run = True
        if args.do_plot is None:
            args.do_plot = True

    if args.do_plot is None:
        args.do_plot = not (args.prepare_only or args.build_only)

    return args


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    if args.list:
        log("Experiments (each uses artifact-evaluation/<dir>/run.sh):")
        for name in PAPER_ORDER + EXTRA_ORDER:
            exp = EXPERIMENTS[name]
            tag = "paper" if exp.is_paper else "extra"
            script = run_script(name)
            present = "run.sh" if script.is_file() else "MISSING run.sh"
            log(f"  {name:<26} {exp.status:<6} [{tag}]  {exp.directory}/{present}")
        return 0

    names = resolve_names(args.mode)

    log("=== One-click AE: artifact-evaluation/run_all.py ===")
    log(f"=== mode: {' '.join(args.mode) or 'ready'} ===")
    log(f"=== experiments: {' '.join(names)} ===")
    log(
        f"=== stages: prepare={args.do_prepare}({args.prepare_mode}) "
        f"build={args.do_build} run={args.do_run} plot={args.do_plot} "
        f"dry_run={args.dry_run} force_build={args.force_base_build} ==="
    )

    if (args.plot_only or args.gather_only) and not args.dry_run:
        log("=== plot-only: re-plot from existing logs (no QEMU) ===")

    if args.dry_run:
        if args.do_prepare:
            log(f"[dry-run] would run: prepare.sh {args.prepare_mode}")
        if args.do_build:
            log("[dry-run] would run: ae_ensure_base_build (common.sh)")
        if args.prepare_only or args.build_only:
            return 0

    if not args.dry_run:
        if args.do_prepare:
            ensure_prepare(skip=False, mode=args.prepare_mode)
            if args.prepare_only:
                log("Prepare-only done.")
                return 0

        if args.do_build:
            ensure_base_build(skip=False, force=args.force_base_build)
            if args.build_only:
                log("Build-only done.")
                return 0

    status: Dict[str, str] = {}
    plot_status: Dict[str, str] = {}
    overall = 0

    if args.do_run:
        for name in names:
            exp = EXPERIMENTS[name]
            status[name] = run_experiment(
                name,
                budget=budget_for(exp, args.budget),
                dry_run=args.dry_run,
            )
            st = status[name]
            if st not in ("OK", "DRY_RUN", "TODO(stub)") and not st.startswith("TODO"):
                overall = 1
            if args.do_plot:
                plot_status[name] = run_plot(name, dry_run=args.dry_run)
                pst = plot_status[name]
                if pst not in ("OK", "DRY_RUN", "TODO(stub)", "NO_INPUT"):
                    overall = 1
    elif args.dry_run:
        for name in names:
            status[name] = run_experiment(
                name,
                budget=budget_for(EXPERIMENTS[name], args.budget),
                dry_run=True,
            )
            if args.do_plot:
                plot_status[name] = run_plot(name, dry_run=True)
    elif args.do_plot:
        for name in names:
            plot_status[name] = run_plot(name, dry_run=args.dry_run)
            pst = plot_status[name]
            if pst not in ("OK", "DRY_RUN", "TODO(stub)", "NO_INPUT"):
                overall = 1

    log("")
    log("#" * 60)
    log("### Summary")
    log("#" * 60)
    ready_ok = ready_total = 0
    for name in names:
        st = status.get(name, "skipped")
        pst = plot_status.get(name, "skipped" if args.do_plot else "-")
        log(f"{name:<26} run={st:<20} plot={pst:<16} {exp_dir(name)}")
        if st not in ("DRY_RUN", "TODO(stub)", "skipped"):
            ready_total += 1
            if st == "OK":
                ready_ok += 1

    log(f"Ready experiments OK: {ready_ok} / {ready_total}")
    return overall


if __name__ == "__main__":
    sys.exit(main())
