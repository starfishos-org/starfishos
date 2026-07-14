#!/usr/bin/env python3
"""One-click StarfishOS artifact evaluation.

Usage (from repository root)::

    python3 artifact-evaluation/run_all.py
    ./artifact-evaluation/run_all.py

First run prepares shared memory / ivshmem and, if needed, builds the OS
(same as ``make prepare``'s quick-build). Then it runs implemented paper
experiments, gathers figures under ``out/<timestamp>/figures/``, and leaves
TODO placeholders for missing paper figures.

Stage control examples::

    # Only prepare the AE environment (ivshmem / hostfs / doorbell)
    ./artifact-evaluation/run_all.py --prepare-only
    ./artifact-evaluation/run_all.py --prepare-only --prepare-mode recreate

    # Only first-time / forced OS build
    ./artifact-evaluation/run_all.py --build-only
    ./artifact-evaluation/run_all.py --build-only --force-base-build

    # Skip prepare/build; only run selected experiments
    ./artifact-evaluation/run_all.py --no-prepare --no-build ready
    ./artifact-evaluation/run_all.py --experiments-only ipc-cdf

    # Gather figures from existing experiment outputs (no QEMU)
    ./artifact-evaluation/run_all.py --gather-only
    ./artifact-evaluation/run_all.py --gather-only ready

    # Full run with tweaks
    ./artifact-evaluation/run_all.py --prepare-mode recreate --force-base-build
    ./artifact-evaluation/run_all.py --budget 7200 ipc-cdf
    ./artifact-evaluation/run_all.py --dry-run
    ./artifact-evaluation/run_all.py --list
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
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


@dataclass(frozen=True)
class Experiment:
    name: str
    directory: str
    status: str  # ready | stub
    budget_s: int
    paper_fig: str
    outputs: str
    is_paper: bool = True


EXPERIMENTS: Dict[str, Experiment] = {
    "basic": Experiment(
        "basic", "0-basic", "ready", 3600,
        "setup table (MLC/MSI)", "CSV/logs (no paper figure)", is_paper=False,
    ),
    "ipc-cdf": Experiment(
        "ipc-cdf", "1-ipc-cdf", "ready", 10800,
        "IPC CDF + breakdown", "ipc_cdf.png, ipc_read_breakdown.png",
    ),
    "sched-notify": Experiment(
        "sched-notify", "2-sched-notify-latency", "ready", 3600,
        "(extra) sched/notify", "sched_notify_latency.png", is_paper=False,
    ),
    "memory-allocator": Experiment(
        "memory-allocator", "3-memory-allocator", "ready", 28800,
        "allocator fig00", "fig00-allocator-all.{png,pdf,eps}",
    ),
    "state-partition": Experiment(
        "state-partition", "4-state-partition", "ready", 36000,
        "state_partition", "fig13-state-partition.{pdf,eps}",
    ),
    "dbx1000-cross-warehouse": Experiment(
        "dbx1000-cross-warehouse", "5-dbx1000-cross-warehouse", "ready", 21600,
        "(extra) cross-warehouse", "dbx1000-cross-warehouse.{pdf,eps}",
        is_paper=False,
    ),
    "auto-scale": Experiment(
        "auto-scale", "6-auto-scale", "stub", 28800,
        "auto-scale-matrix / db1000 / gemini",
        "TODO: auto-scale-matrix.eps, db1000.eps, gemini-chcore.eps",
    ),
    "recover-fs": Experiment(
        "recover-fs", "7-recover-fs", "ready", 10800,
        "recovery-performance-single", "recovery-performance.{png,pdf}",
    ),
    "process-migration": Experiment(
        "process-migration", "8-process-migration", "stub", 14400,
        "process-migration",
        "TODO: process-migration-data-large/small.eps",
    ),
    "resource-util": Experiment(
        "resource-util", "9-resource-util", "stub", 21600,
        "real.eps", "TODO: real.eps",
    ),
}

FIXED_DIR_FIGURES = {
    "ipc-cdf": [
        "ipc_cdf.png", "ipc_read_breakdown.png",
    ],
    "sched-notify": ["sched_notify_latency.png"],
    "memory-allocator": [
        "fig00-allocator-all.png",
        "fig00-allocator-all.pdf",
        "fig00-allocator-all.eps",
        "allocator_overview.png",
        "allocator_cxl.png",
        "user_malloc.png",
    ],
}

AE_SESSIONS = [
    "{user}-ae",
    "{user}-ipc-ae",
    "{user}-sched-notify-ae",
    "{user}-recover-fs-ae",
    "{user}-msi-basic-ae",
    "{user}-qemu",
]


def log(msg: str = "") -> None:
    print(msg, flush=True)


def budget_for(exp: Experiment, global_budget: Optional[int]) -> int:
    key = f"BUDGET_{exp.name.upper().replace('-', '_')}"
    raw = os.environ.get(key)
    if raw:
        return int(raw)
    if global_budget is not None:
        return global_budget
    return exp.budget_s


def resolve_names(mode_args: List[str]) -> List[str]:
    if not mode_args or mode_args == ["paper"]:
        return list(PAPER_ORDER)
    head = mode_args[0]
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
    user = os.environ.get("USER", "user")
    for tmpl in AE_SESSIONS:
        name = tmpl.format(user=user)
        subprocess.run(
            ["tmux", "kill-session", "-t", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def run_cmd(
    cmd: List[str],
    *,
    cwd: Path,
    log_path: Optional[Path] = None,
    timeout: Optional[int] = None,
    env: Optional[dict] = None,
) -> int:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    stdout = None
    stderr = None
    handle = None
    try:
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            handle = log_path.open("w", encoding="utf-8")
            stdout = handle
            stderr = subprocess.STDOUT
        return subprocess.run(
            cmd,
            cwd=str(cwd),
            stdout=stdout,
            stderr=stderr,
            timeout=timeout,
            env=merged,
            check=False,
        ).returncode
    except subprocess.TimeoutExpired:
        return 124
    finally:
        if handle is not None:
            handle.close()


def ensure_prepare(
    log_dir: Path,
    *,
    skip: bool,
    mode: str = "ensure",
) -> None:
    if skip:
        log("=== Skipping prepare.sh (--no-prepare / --skip-prepare) ===")
        return
    log(f"=== Global preparation (prepare.sh {mode}) ===")
    rc = run_cmd(
        [str(AE_ROOT / "prepare.sh"), mode],
        cwd=REPO_ROOT,
        log_path=log_dir / "prepare.log",
    )
    if rc != 0:
        raise SystemExit(
            f"prepare.sh {mode} failed (rc={rc}); see {log_dir / 'prepare.log'}"
        )
    log(f"[AE] prepare.sh {mode} OK")


def ensure_base_build(log_dir: Path, *, skip: bool, force: bool) -> None:
    if skip:
        log("=== Skipping first-time OS build (--no-build / --skip-base-build) ===")
        return

    log("=== Ensuring first-time OS build (make prepare equivalent, if needed) ===")
    config = REPO_ROOT / ".config"
    kernel = REPO_ROOT / "build" / "kernel.img"
    quick = REPO_ROOT / "scripts" / "quick-build.sh"
    build_log = log_dir / "base_build.log"

    lines: List[str] = []
    if force:
        lines.append("[AE] FORCE_BASE_BUILD=1 — running scripts/quick-build.sh")
        need = True
    elif config.is_file() and kernel.is_file():
        lines.append(
            "[AE] OS already prepared (.config + build/kernel.img present); "
            "skip first-time build"
        )
        need = False
    else:
        lines.append("[AE] First-time OS prepare (same as make prepare's build step)")
        if not config.is_file():
            lines.append("[AE]   missing .config")
        if not kernel.is_file():
            lines.append(f"[AE]   missing {kernel}")
        need = True

    build_log.write_text("\n".join(lines) + "\n", encoding="utf-8")
    for line in lines:
        log(line)

    if not need:
        return

    with build_log.open("a", encoding="utf-8") as handle:
        proc = subprocess.run(
            [str(quick)],
            cwd=str(REPO_ROOT),
            stdout=handle,
            stderr=subprocess.STDOUT,
            check=False,
        )
    if proc.returncode != 0 or not kernel.is_file():
        raise SystemExit(f"First-time OS prepare failed; see {build_log}")
    log(f"[AE] First-time OS prepare done: {kernel}")


def latest_out_dir(ae_dir: Path) -> Optional[Path]:
    out = ae_dir / "out"
    if not out.is_dir():
        return None
    candidates = sorted(
        [p for p in out.iterdir() if p.is_dir()],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def gather_figures(name: str, dest_root: Path, outdirs: Dict[str, Path]) -> None:
    exp = EXPERIMENTS[name]
    ae_dir = AE_ROOT / exp.directory
    dest = dest_root / name
    dest.mkdir(parents=True, exist_ok=True)
    copied = 0

    def copy_file(src: Path) -> None:
        nonlocal copied
        if src.is_file():
            shutil.copy2(src, dest / src.name)
            copied += 1

    if name in FIXED_DIR_FIGURES:
        for fname in FIXED_DIR_FIGURES[name]:
            copy_file(ae_dir / fname)
        outdirs[name] = ae_dir
    elif name in ("state-partition", "dbx1000-cross-warehouse"):
        latest = latest_out_dir(ae_dir)
        if latest is not None:
            outdirs[name] = latest
            fig_dir = latest / "figures"
            if fig_dir.is_dir():
                for src in fig_dir.iterdir():
                    copy_file(src)
    elif name == "recover-fs":
        latest = latest_out_dir(ae_dir)
        if latest is not None:
            outdirs[name] = latest
            for src in latest.glob("recovery-performance.*"):
                copy_file(src)
            fig_dir = latest / "figures"
            if fig_dir.is_dir():
                for src in fig_dir.iterdir():
                    copy_file(src)
    elif name == "basic":
        outdirs[name] = ae_dir
        for pattern in ("*.csv", "*.png", "*.pdf", "*.log"):
            for src in ae_dir.glob(pattern):
                copy_file(src)
            for sub in ae_dir.glob("*"):
                if sub.is_dir():
                    for src in sub.glob(pattern):
                        copy_file(src)
    else:
        latest = latest_out_dir(ae_dir)
        if latest is not None:
            outdirs[name] = latest
            for pattern in ("*.png", "*.pdf", "*.eps"):
                for src in latest.rglob(pattern):
                    copy_file(src)

    if copied == 0:
        log(f"[run-all] WARNING: no figures gathered for {name} under {dest}")
    else:
        log(f"[run-all] gathered {copied} file(s) for {name} → {dest}")


def write_todo_placeholder(name: str, fig_dir: Path) -> None:
    exp = EXPERIMENTS[name]
    dest = fig_dir / name
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "TODO.md").write_text(
        "\n".join(
            [
                f"# TODO: {name}",
                "",
                f"Paper figure: {exp.paper_fig}",
                "",
                f"Expected outputs: {exp.outputs}",
                "",
                f"Implement: `artifact-evaluation/{exp.directory}/run.sh`",
                "",
                f"See also: `artifact-evaluation/{exp.directory}/README.md` "
                "and `p3os-paper/eval/`.",
                "",
            ]
        ),
        encoding="utf-8",
    )
    log(f"[run-all] TODO: stub {exp.directory} — placeholder at {dest / 'TODO.md'}")


def write_figures_report(
    top_out: Path,
    fig_dir: Path,
    status: Dict[str, str],
    *,
    dry_run: bool,
) -> None:
    report = top_out / "FIGURES.md"
    lines = [
        f"# Paper figures — {datetime.now().astimezone().isoformat()}",
        "",
        f"Output directory: `{top_out}`",
        "",
        "| Experiment | Status | Paper figure | Outputs |",
        "| --- | --- | --- | --- |",
    ]
    for name in PAPER_ORDER:
        exp = EXPERIMENTS[name]
        if exp.status == "stub":
            line_status = "TODO"
        else:
            st = status.get(name, "TODO")
            if st == "OK":
                line_status = "OK"
            elif st in ("DRY_RUN", "GATHERED"):
                line_status = st
            elif st.startswith("TODO") or st.startswith("STUB"):
                line_status = "TODO"
            else:
                line_status = st
        lines.append(
            f"| `{name}` (`{exp.directory}`) | **{line_status}** | "
            f"{exp.paper_fig} | {exp.outputs} |"
        )

    lines.extend(["", "## TODO (not implemented yet)", ""])
    for name in PAPER_ORDER:
        exp = EXPERIMENTS[name]
        if exp.status == "stub":
            lines.append(f"- [ ] **{name}**: {exp.outputs}")
            lines.append(f"  - paper: {exp.paper_fig}")
            lines.append(
                f"  - implement `artifact-evaluation/{exp.directory}/run.sh`"
            )

    lines.extend(["", "## Generated files", "", "```"])
    files = sorted(str(p) for p in fig_dir.rglob("*") if p.is_file())
    lines.extend(files or ["(none)"])
    lines.extend(["```", ""])
    report.write_text("\n".join(lines), encoding="utf-8")
    log(f"[run-all] wrote {report}")

    if not dry_run:
        shutil.copy2(report, AE_ROOT / "TODO-FIGURES.md")
        log(f"[run-all] updated {AE_ROOT / 'TODO-FIGURES.md'}")


def run_one(
    name: str,
    *,
    fig_dir: Path,
    log_dir: Path,
    dry_run: bool,
    status: Dict[str, str],
    outdirs: Dict[str, Path],
    global_budget: Optional[int],
    do_gather: bool,
) -> None:
    exp = EXPERIMENTS[name]
    budget = budget_for(exp, global_budget)
    script = AE_ROOT / exp.directory / ("run_msi.sh" if name == "basic" else "run.sh")

    log("")
    log("#" * 60)
    log(f"### [{time.strftime('%H:%M:%S')}] {name}")
    log(f"###   dir={exp.directory}  status={exp.status}  budget={budget}s")
    log(f"###   paper: {exp.paper_fig}")
    log("#" * 60)

    if exp.status == "stub":
        status[name] = "TODO(not implemented)"
        write_todo_placeholder(name, fig_dir)
        return

    if not script.is_file() and not (
        name == "basic" and (AE_ROOT / exp.directory / "run_mlc.sh").is_file()
    ):
        status[name] = f"MISSING({script})"
        log(f"[run-all] missing entry script: {script}")
        return

    if dry_run:
        status[name] = "DRY_RUN"
        if name == "basic":
            log(
                f"[dry-run] would run: {AE_ROOT / exp.directory / 'run_msi.sh'} ; "
                f"{AE_ROOT / exp.directory / 'run_mlc.sh'} (MLC optional)"
            )
        else:
            log(f"[dry-run] would run: {script}  (timeout {budget}s)")
        return

    kill_ae_sessions()
    exp_log = log_dir / f"{name}.log"

    if name == "basic":
        rc = run_cmd(
            ["timeout", "--kill-after=60", str(budget), str(script)],
            cwd=REPO_ROOT,
            log_path=exp_log,
        )
        if rc == 0:
            mlc = AE_ROOT / exp.directory / "run_mlc.sh"
            with exp_log.open("a", encoding="utf-8") as handle:
                subprocess.run(
                    [str(mlc)],
                    cwd=str(REPO_ROOT),
                    stdout=handle,
                    stderr=subprocess.STDOUT,
                    env={**os.environ, "ALLOW_MLC_SKIP": "1"},
                    check=False,
                )
    else:
        rc = run_cmd(
            ["timeout", "--kill-after=60", str(budget), str(script)],
            cwd=REPO_ROOT,
            log_path=exp_log,
        )

    if rc == 0:
        status[name] = "OK"
        if do_gather:
            gather_figures(name, fig_dir, outdirs)
    elif rc in (124, 137):
        status[name] = f"TIMEOUT({budget}s)"
        log(f"[run-all][TIMEOUT] {name} exceeded {budget}s")
        kill_ae_sessions()
    elif rc == 2:
        status[name] = "FAILED(step-timeouts)"
        if do_gather:
            gather_figures(name, fig_dir, outdirs)
    else:
        status[name] = f"FAILED(rc={rc})"
        log(f"[run-all] {name} FAILED; last 20 log lines:")
        try:
            lines = exp_log.read_text(encoding="utf-8", errors="replace").splitlines()
            for line in lines[-20:]:
                log(line)
        except OSError:
            pass
        kill_ae_sessions()


def env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="One-click StarfishOS artifact evaluation (tests + figures).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "mode",
        nargs="*",
        default=["paper"],
        help="paper (default) | ready | all | extra | experiment names",
    )

    stages = parser.add_argument_group("stage shortcuts (exit after the stage)")
    stages.add_argument(
        "--prepare-only",
        action="store_true",
        help="Only run prepare.sh, then exit",
    )
    stages.add_argument(
        "--build-only",
        action="store_true",
        help="Only run first-time/forced OS build, then exit",
    )
    stages.add_argument(
        "--gather-only",
        action="store_true",
        help="Only gather existing figures into out/<ts>/figures/, no QEMU",
    )
    stages.add_argument(
        "--experiments-only",
        action="store_true",
        help="Skip prepare + OS build; only run experiments (and gather)",
    )

    prepare_g = parser.add_argument_group("prepare (shared memory / ivshmem)")
    prepare_g.add_argument(
        "--prepare",
        dest="do_prepare",
        action="store_true",
        default=None,
        help="Run prepare.sh (default on for full runs)",
    )
    prepare_g.add_argument(
        "--no-prepare",
        "--skip-prepare",
        dest="do_prepare",
        action="store_false",
        help="Skip prepare.sh",
    )
    prepare_g.add_argument(
        "--prepare-mode",
        choices=("ensure", "recreate"),
        default=os.environ.get("PREPARE_MODE", "ensure"),
        help="prepare.sh mode: ensure (default, idempotent) or recreate",
    )

    build_g = parser.add_argument_group("OS build (make prepare compile step)")
    build_g.add_argument(
        "--build",
        dest="do_build",
        action="store_true",
        default=None,
        help="Ensure first-time OS build (default on for full runs)",
    )
    build_g.add_argument(
        "--no-build",
        "--skip-base-build",
        dest="do_build",
        action="store_false",
        help="Skip OS build check",
    )
    build_g.add_argument(
        "--force-base-build",
        action="store_true",
        default=env_flag("FORCE_BASE_BUILD"),
        help="Force scripts/quick-build.sh even if kernel.img exists",
    )

    run_g = parser.add_argument_group("experiments / figures")
    run_g.add_argument(
        "--run",
        dest="do_run",
        action="store_true",
        default=None,
        help="Run experiment scripts (default on)",
    )
    run_g.add_argument(
        "--no-run",
        dest="do_run",
        action="store_false",
        help="Do not launch experiment QEMU runs",
    )
    run_g.add_argument(
        "--gather",
        dest="do_gather",
        action="store_true",
        default=None,
        help="Gather figures after runs (default on)",
    )
    run_g.add_argument(
        "--no-gather",
        dest="do_gather",
        action="store_false",
        help="Do not copy figures into out/<ts>/figures/",
    )
    run_g.add_argument(
        "--budget",
        type=int,
        default=None,
        help="Override wall-clock budget (seconds) for all ready experiments",
    )
    run_g.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output directory (default: artifact-evaluation/out/<timestamp>)",
    )

    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=env_flag("DRY_RUN"),
        help="Print plan only; do not prepare, build, or boot QEMU",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List experiments and exit",
    )

    args = parser.parse_args(argv)

    # Resolve stage defaults after parsing (None → True unless shortcut overrides).
    if args.prepare_only:
        args.do_prepare = True
        args.do_build = False
        args.do_run = False
        args.do_gather = False
    elif args.build_only:
        args.do_prepare = False if args.do_prepare is None else args.do_prepare
        args.do_build = True
        args.do_run = False
        args.do_gather = False
    elif args.gather_only:
        args.do_prepare = False
        args.do_build = False
        args.do_run = False
        args.do_gather = True
    elif args.experiments_only:
        args.do_prepare = False
        args.do_build = False
        args.do_run = True if args.do_run is None else args.do_run
        args.do_gather = True if args.do_gather is None else args.do_gather
    else:
        if args.do_prepare is None:
            args.do_prepare = not env_flag("SKIP_PREPARE")
        if args.do_build is None:
            args.do_build = not env_flag("SKIP_BASE_BUILD")
        if args.do_run is None:
            args.do_run = True
        if args.do_gather is None:
            args.do_gather = True

    return args


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    if args.list:
        log("Known experiments:")
        for name in PAPER_ORDER + EXTRA_ORDER:
            exp = EXPERIMENTS[name]
            tag = "paper" if exp.is_paper else "extra"
            log(f"  {name:<26} {exp.status:<6} [{tag}]  {exp.paper_fig}")
        return 0

    names = resolve_names(args.mode)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    top_out = Path(args.out) if args.out else (AE_ROOT / "out" / ts)
    if not top_out.is_absolute():
        top_out = (REPO_ROOT / top_out).resolve()
    fig_dir = top_out / "figures"
    log_dir = top_out / "logs"
    fig_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    log("=== One-click AE: python3 artifact-evaluation/run_all.py ===")
    log(f"=== mode: {' '.join(args.mode) or 'paper'} ===")
    log(f"=== experiments: {' '.join(names)} ===")
    log(f"=== output: {top_out} ===")
    log(
        f"=== stages: prepare={args.do_prepare}({args.prepare_mode}) "
        f"build={args.do_build} run={args.do_run} gather={args.do_gather} "
        f"dry_run={args.dry_run} force_build={args.force_base_build} ==="
    )

    status: Dict[str, str] = {}
    outdirs: Dict[str, Path] = {}

    if args.dry_run:
        log("=== Skipping prepare/build/run side effects (dry-run) ===")
        if args.do_prepare:
            log(f"[dry-run] would run: prepare.sh {args.prepare_mode}")
        if args.do_build:
            log("[dry-run] would ensure OS build (quick-build if needed)")
        if args.prepare_only or args.build_only:
            return 0
        for name in names:
            if EXPERIMENTS[name].status == "stub":
                status[name] = "TODO(not implemented)"
                write_todo_placeholder(name, fig_dir)
            elif args.do_run:
                status[name] = "DRY_RUN"
                log(
                    f"[dry-run] would run: "
                    f"{AE_ROOT / EXPERIMENTS[name].directory / 'run.sh'}"
                )
            elif args.do_gather:
                status[name] = "DRY_RUN"
                log(f"[dry-run] would gather figures for {name}")
        write_figures_report(top_out, fig_dir, status, dry_run=True)
        log("Dry-run complete.")
        return 0

    if args.do_prepare:
        ensure_prepare(log_dir, skip=False, mode=args.prepare_mode)
        if args.prepare_only:
            log("Prepare-only done.")
            return 0

    if args.do_build:
        ensure_base_build(
            log_dir,
            skip=False,
            force=args.force_base_build,
        )
        if args.build_only:
            log("Build-only done.")
            return 0

    if args.do_run:
        for name in names:
            run_one(
                name,
                fig_dir=fig_dir,
                log_dir=log_dir,
                dry_run=False,
                status=status,
                outdirs=outdirs,
                global_budget=args.budget,
                do_gather=args.do_gather,
            )
    elif args.do_gather:
        log("=== Gather-only: copying existing figures ===")
        for name in names:
            if EXPERIMENTS[name].status == "stub":
                status[name] = "TODO(not implemented)"
                write_todo_placeholder(name, fig_dir)
            else:
                gather_figures(name, fig_dir, outdirs)
                status[name] = "GATHERED"
    else:
        log("=== No run/gather stages requested ===")

    log("")
    log("#" * 60)
    log("### Summary")
    log("#" * 60)
    overall = 0
    ready_ok = 0
    ready_total = 0
    for name in names:
        st = status.get(name, "skipped")
        log(f"{name:<26} {st:<28} {outdirs.get(name, '')}")
        if st.startswith("TODO") or st.startswith("STUB") or st in (
            "DRY_RUN",
            "GATHERED",
            "skipped",
        ):
            if st == "GATHERED":
                ready_ok += 1
                ready_total += 1
            continue
        ready_total += 1
        if st == "OK":
            ready_ok += 1
        else:
            overall = 1

    if args.do_gather or args.do_run:
        write_figures_report(top_out, fig_dir, status, dry_run=False)

    log("")
    log(f"Figures directory: {fig_dir}")
    for path in sorted(fig_dir.rglob("*")):
        if path.is_file():
            log(f"  {path.relative_to(fig_dir)}")
    log("")
    log(f"Paper figure checklist: {top_out / 'FIGURES.md'}")
    log(f"Stable TODO list:      {AE_ROOT / 'TODO-FIGURES.md'}")
    log(f"Ready experiments OK:  {ready_ok} / {ready_total} (TODOs excluded)")
    return overall


if __name__ == "__main__":
    sys.exit(main())
