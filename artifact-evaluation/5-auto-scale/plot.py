#!/usr/bin/env python3
"""Parse auto-scaling sweep logs and plot the three paper auto-scale figures:

  auto-scale-matrix.png  — Matrix-multiply MapReduce, throughput vs #machines
  db1000.png             — DBx1000 TPC-C throughput vs #machines
  gemini-chcore.png      — GeminiGraph PageRank runtime->throughput vs #machines

Each figure compares StarfishOS (Mixed / CXL) against external baselines
(Ideal = Linux DRAM, Distributed = Linux-MPI, Tigon).  The StarfishOS curves
come from ChCore sweep logs; the baselines come from `test-on-linux/` ports.

Data-file formats (identical to p3os-paper/eval/):
  matrix : lines `RESULT: N=<m> CONFIG=<MIXED|CXL|TCP|IDEAL> TIME=<us>`
  db1000 : CSV `module,machines,performance_mops`
           modules P3OS-mixed, P3OS-all_cxl, tigon, linux
  gemini : CSV `machines,MIXED_DEFAULT_CXL,CXL,DRAM,LINUX-DRAM,DISTRIBUTED`
           (values are `<seconds>s`)

Sweep logs produced by run.sh (`--log-dir`):
  <app>_<Mixed|CXL>_N<n>.log

Draw from data files (verify against paper)::

  python3 plot.py --csv-dir /tmp/as-check/csv \
                  --fig-dir /tmp/as-check/figures \
    --matrix-data /path/to/paper/4000size.txt \
    --db1000-data /path/to/paper/db1000-p3os-tigon.csv \
    --gemini-data /path/to/paper/gemini-data.log

Collect StarfishOS curves from a sweep then plot::

  python3 plot.py --log-dir logs --csv-dir csv --fig-dir figures
"""
from __future__ import annotations

import argparse
import math
import re
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.lines import Line2D
from matplotlib.ticker import MultipleLocator

SYSTEM_NAME = "Starfish"
X_TICKS = [1, 2, 4, 6, 8]

LOG_NAME_RE = re.compile(
    r"^(?P<app>matrix|db1000|gemini)_"
    r"(?P<config>Mixed|CXL|Ideal|Distributed|Tigon)_N(?P<n>\d+)\.log$"
)
AE_RESULT_RE = re.compile(
    r"AE_RESULT\s+app=(\w+)\s+series=(\w+)\s+n=(\d+)\s+"
    r"value=([\d.eE+-]+)\s+unit=(\w+)"
)
PAT_PHOENIX_US = re.compile(r"(?<!inter )library:\s*([\d.]+)")
PAT_FINALIZE_US = re.compile(r"finalize:\s*([\d.]+)")
PAT_THP = re.compile(r"thp=([\d.eE+-]+)")
PAT_EXEC_TIME = re.compile(r"exec_time[:=]\s*([\d.]+)")
FLOAT_PATTERN = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
GEMINI_SAMPLE_RE = re.compile(
    rf"^exec_time=({FLOAT_PATTERN})\(s\)\r?\n"
    rf"pr_sum=({FLOAT_PATTERN})\r?\n"
    rf"pr\[(\d+)\]=({FLOAT_PATTERN})\r?$",
    re.MULTILINE,
)
# run.sh fixes the workload to twitter-2010.bin with 41,652,230 vertices and
# 50 PageRank iterations.  Linux implementations differ slightly at high
# thread counts, so this is a correctness corridor, not an equality check.
GEMINI_VERTEX_COUNT = 41_652_230
GEMINI_REFERENCE_PR_SUM = 38_256_454.72
GEMINI_REFERENCE_MAX_VERTEX = 21_513_299
GEMINI_REFERENCE_MAX_VALUE = 14_068.815162
GEMINI_CORRECTNESS_REL_TOL = 1e-3

CONFIG_TO_MATRIX = {
    "Mixed": "MIXED", "CXL": "CXL", "Ideal": "IDEAL",
    "Distributed": "TCP",
}
CONFIG_TO_DB1000 = {
    "Mixed": "P3OS-mixed", "CXL": "P3OS-all_cxl",
    "Ideal": "linux", "Tigon": "tigon",
}


def _draw_fig(fig_dir: Path, name: str):
    fig_dir.mkdir(parents=True, exist_ok=True)
    stem = fig_dir / name
    plt.savefig(stem.with_suffix(".png"), dpi=200, format="png", bbox_inches="tight")
    print(f"Wrote {stem}.png")


def draw_legend(fig_dir: Path):
    """Draw the shared legend included separately by the paper's TeX source."""
    fig_dir.mkdir(parents=True, exist_ok=True)
    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    handles = [
        Line2D([0], [0], color="#d62728", marker="x", linestyle="-", linewidth=2.2, markersize=8, markeredgewidth=2),
        Line2D([0], [0], color="#1f77b4", marker="s", linestyle="-.", linewidth=2.2, markersize=7, markeredgewidth=1.8),
        Line2D([0], [0], color="black", marker="o", linestyle="--", linewidth=2.2, markersize=6, markeredgewidth=1.5),
        Line2D([0], [0], color="#ff7f0e", marker="^", linestyle=":", linewidth=2.2, markersize=7, markeredgewidth=1.5),
        Line2D([0], [0], color="silver", marker="d", linestyle="--", linewidth=2.2, markersize=6, markeredgewidth=1.5),
    ]
    labels = ["Starfish Mixed", "Starfish CXL", "Distributed", "Tigon", "Ideal"]
    fig = plt.figure(figsize=(8.4, 0.52))
    axis = fig.add_subplot(111)
    axis.axis("off")
    fig.legend(handles, labels, loc="center", bbox_to_anchor=(0.5, 0.5), ncol=5,
               frameon=False, fontsize=15, handlelength=1.15,
               columnspacing=0.75, handletextpad=0.3)
    axis.set_position([0, 0, 1, 1])
    fig.tight_layout(pad=0)
    stem = fig_dir / "auto-scale-legend"
    fig.savefig(stem.with_suffix(".png"), dpi=200, format="png", bbox_inches="tight", pad_inches=0.001)
    plt.close(fig)
    print(f"Wrote {stem}.png")


def _last_float(pattern: re.Pattern, text: str) -> Optional[float]:
    val = None
    for m in pattern.finditer(text):
        val = float(m.group(1))
    return val


def _extract_matrix_us(text: str) -> Optional[float]:
    return _last_float(PAT_PHOENIX_US, text) or _last_float(PAT_FINALIZE_US, text)


def _extract_db1000_thp(text: str) -> Optional[float]:
    return _last_float(PAT_THP, text)


def _extract_gemini_secs(text: str) -> Optional[float]:
    return _last_float(PAT_EXEC_TIME, text)


def _relative_deviation(value: float, reference: float) -> float:
    return abs(value - reference) / abs(reference)


def _valid_gemini_samples(text: str, path_name: str) -> bool:
    """Require complete, numerically sane PageRank records for this dataset."""
    samples = [
        (float(seconds), float(pr_sum), int(max_vertex), float(max_value))
        for seconds, pr_sum, max_vertex, max_value
        in GEMINI_SAMPLE_RE.findall(text)
    ]
    if not samples:
        print(f"[WARN] no complete Gemini timing/checksum record in {path_name}")
        return False

    for sample_index, (seconds, pr_sum, max_vertex, max_value) in enumerate(
        samples, start=1
    ):
        finite = all(math.isfinite(value) for value in (seconds, pr_sum, max_value))
        sum_deviation = (
            _relative_deviation(pr_sum, GEMINI_REFERENCE_PR_SUM)
            if finite else float("inf")
        )
        max_deviation = (
            _relative_deviation(max_value, GEMINI_REFERENCE_MAX_VALUE)
            if finite else float("inf")
        )
        if (not finite or seconds <= 0 or pr_sum <= 0
                or pr_sum > GEMINI_VERTEX_COUNT * (1 + GEMINI_CORRECTNESS_REL_TOL)
                or not 0 <= max_vertex < GEMINI_VERTEX_COUNT
                or max_vertex != GEMINI_REFERENCE_MAX_VERTEX
                or max_value <= 0 or max_value > pr_sum
                or sum_deviation > GEMINI_CORRECTNESS_REL_TOL
                or max_deviation > GEMINI_CORRECTNESS_REL_TOL):
            print(
                f"[WARN] invalid Gemini PageRank result in {path_name} "
                f"sample={sample_index}: seconds={seconds:g} pr_sum={pr_sum:g} "
                f"max_vertex={max_vertex} max_value={max_value:g}"
            )
            return False
    return True


def collect_from_logs(log_dir: Path, results_dir: Path,
                      required_logs: Iterable[str] = ()) -> Dict[str, Path]:
    """Convert run.sh sweep logs into the three plotter data files.

    Returns a dict of logical name -> written path for files that were produced.
    """
    results_dir.mkdir(parents=True, exist_ok=True)

    matrix_lines: List[str] = []
    db1000_rows: List[Tuple[str, int, float]] = []
    parsed_logs: Set[str] = set()
    # machine -> {Mixed: secs, CXL: secs}
    gemini: Dict[int, Dict[str, float]] = {}

    for path in sorted(log_dir.glob("*.log")):
        m = LOG_NAME_RE.match(path.name)
        if not m:
            continue
        app, config, n = m.group("app"), m.group("config"), int(m.group("n"))
        text = path.read_text(errors="replace")
        expected_unit = {"matrix": "us", "db1000": "mops", "gemini": "s"}[app]
        standardized = [
            result for result in AE_RESULT_RE.finditer(text)
            if result.group(1) == app
            and result.group(2) == config
            and int(result.group(3)) == n
            and result.group(5) == expected_unit
        ]
        std_value = float(standardized[-1].group(4)) if standardized else None
        if app == "matrix" and config in CONFIG_TO_MATRIX:
            us = std_value if std_value is not None else _extract_matrix_us(text)
            if us is None:
                print(f"[WARN] no matrix timing in {path.name}")
                continue
            matrix_lines.append(
                f"RESULT: N={n} CONFIG={CONFIG_TO_MATRIX[config]} TIME={int(us)}"
            )
            parsed_logs.add(path.name)
        elif app == "db1000" and config in CONFIG_TO_DB1000:
            thp = std_value if std_value is not None else _extract_db1000_thp(text)
            if thp is None:
                print(f"[WARN] no thp= in {path.name}")
                continue
            db1000_rows.append((CONFIG_TO_DB1000[config], n, thp))
            parsed_logs.add(path.name)
        elif app == "gemini":
            if not _valid_gemini_samples(text, path.name):
                continue
            secs = std_value if std_value is not None else _extract_gemini_secs(text)
            if secs is None or not math.isfinite(secs) or secs <= 0:
                print(f"[WARN] no exec_time in {path.name}")
                continue
            gemini.setdefault(n, {})[config] = secs
            parsed_logs.add(path.name)

    missing_required = sorted(set(required_logs) - parsed_logs)
    if missing_required:
        raise SystemExit(
            "Missing or unparseable requested sweep logs: "
            + ", ".join(missing_required)
        )

    written: Dict[str, Path] = {}

    if matrix_lines:
        out = results_dir / "4000size.txt"
        out.write_text("\n".join(matrix_lines) + "\n")
        print(f"Wrote {out} ({len(matrix_lines)} RESULT lines)")
        written["matrix"] = out

    if db1000_rows:
        out = results_dir / "db1000-p3os-tigon.csv"
        lines = ["module,machines,performance_mops"]
        for module, machines, perf in sorted(db1000_rows, key=lambda r: (r[0], r[1])):
            lines.append(f"{module},{machines},{perf}")
        out.write_text("\n".join(lines) + "\n")
        print(f"Wrote {out} ({len(db1000_rows)} rows)")
        written["db1000"] = out

    if gemini:
        out = results_dir / "gemini-data.log"
        lines = [
            "machines,MIXED_DEFAULT_CXL,CXL,DRAM,LINUX-DRAM,DISTRIBUTED",
        ]
        for n in sorted(gemini):
            def sec(name):
                value = gemini[n].get(name)
                return f"{value}s" if value is not None else ""
            lines.append(
                f"{n},{sec('Mixed')},{sec('CXL')},,{sec('Ideal')},{sec('Distributed')}"
            )
        out.write_text("\n".join(lines) + "\n")
        print(f"Wrote {out} ({len(gemini)} machine counts)")
        written["gemini"] = out

    return written


# ── auto-scale-matrix ───────────────────────────────────────────────────────

def _missing_points(series, required):
    return [f"{name}/N={n}" for name in required for n in X_TICKS
            if n not in {point[0] for point in series.get(name, [])}]


def draw_matrix(data_path: Path, fig_dir: Path, allow_partial=False):
    pattern = re.compile(r"RESULT:\s+N=(\d+)\s+CONFIG=(\w+)\s+TIME=(\d+)")
    data = {"MIXED": [], "CXL": [], "TCP": [], "IDEAL": []}
    for line in Path(data_path).read_text().splitlines():
        m = pattern.search(line)
        if not m:
            continue
        n, cfg, rt = int(m.group(1)), m.group(2), int(m.group(3))
        if cfg in data:
            data[cfg].append((n, 100 * (1 / (rt / 1000 / 1000))))
    for cfg in data:
        data[cfg].sort(key=lambda it: it[0])
    missing = _missing_points(data, ["MIXED", "CXL", "TCP", "IDEAL"])
    if missing and not allow_partial:
        raise SystemExit("Incomplete Matrix dataset: " + ", ".join(missing))

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (3.8, 4.16)})
    fig, ax1 = plt.subplots()
    styles = {
        "MIXED": {"color": "#d62728", "marker": "x", "linestyle": "-", "label": f"{SYSTEM_NAME} Mixed"},
        "CXL": {"color": "#1f77b4", "marker": "s", "linestyle": "-.", "label": f"{SYSTEM_NAME} CXL"},
        "TCP": {"color": "black", "marker": "o", "linestyle": "--", "label": "Distributed"},
        "IDEAL": {"color": "silver", "marker": "d", "linestyle": "--", "label": "Ideal"},
    }
    for cfg in ["MIXED", "CXL", "TCP", "IDEAL"]:
        pts = [it for it in data[cfg] if it[0] in X_TICKS]
        if not pts:
            continue
        s = styles[cfg]
        ax1.plot([p[0] for p in pts], [p[1] for p in pts], label=s["label"],
                 color=s["color"], linestyle=s["linestyle"], marker=s["marker"],
                 markersize=8, markeredgewidth=2, linewidth=2.2)
    ax1.set_ylabel("1/Run Time (1/s * 100)")
    ax1.set_xticks(X_TICKS)
    ax1.set_xlabel("#Machines")
    ax1.set_ylim(0, 9)
    ax1.yaxis.set_major_locator(MultipleLocator(2))
    ax1.grid(True, which="both", axis="both", linestyle=":")
    fig.tight_layout()
    _draw_fig(fig_dir, "auto-scale-matrix")
    plt.close(fig)


# ── db1000 ──────────────────────────────────────────────────────────────────

def draw_db1000(data_path: Path, fig_dir: Path, allow_partial=False):
    data = pd.read_csv(data_path).sort_values(["module", "machines"])
    expected = {"P3OS-mixed", "P3OS-all_cxl", "tigon", "linux"}
    missing = [f"{module}/N={n}" for module in sorted(expected) for n in X_TICKS
               if not ((data["module"] == module) & (data["machines"] == n)).any()]
    if missing and not allow_partial:
        raise SystemExit("Incomplete DBx1000 dataset: " + ", ".join(missing))
    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (3.8, 4.16)})
    fig, ax1 = plt.subplots()
    styles = {
        "P3OS-mixed": {"color": "#d62728", "marker": "x", "linestyle": "-", "label": SYSTEM_NAME + " Mixed"},
        "P3OS-all_cxl": {"color": "#1f77b4", "marker": "s", "linestyle": "-.", "label": SYSTEM_NAME + " CXL"},
        "tigon": {"color": "#ff7f0e", "marker": "^", "linestyle": ":", "label": "Tigon"},
        "linux": {"color": "silver", "marker": "d", "linestyle": "--", "label": "Ideal"},
    }
    for module, group in data.groupby("module"):
        group = group.sort_values("machines")
        s = styles.get(module, {"color": "#1f77b4", "marker": "s", "linestyle": "-", "label": module})
        ax1.plot(group["machines"].values, group["performance_mops"].values,
                 label=s["label"], color=s["color"], linestyle=s["linestyle"],
                 marker=s["marker"], markersize=8, markeredgewidth=2, linewidth=2.2)
    ax1.set_ylabel("Thp (Mops/s)")
    ax1.set_xlabel("#Machines")
    ax1.set_xticks(sorted(data["machines"].unique()))
    ax1.set_ylim(0)
    ax1.grid(True, which="both", axis="both", linestyle=":")
    fig.tight_layout()
    _draw_fig(fig_dir, "db1000")
    plt.close(fig)


# ── gemini-chcore ───────────────────────────────────────────────────────────

def _parse_secs(s):
    s = (s or "").strip()
    if not s:
        return None
    return float(s.replace("s", ""))


def draw_gemini(data_path: Path, fig_dir: Path, allow_partial=False):
    rows = []
    for line in Path(data_path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(("PageRank", "Graph:", "Date:", "machines")):
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        mixed = _parse_secs(parts[1] if len(parts) > 1 else "")
        cxl = _parse_secs(parts[2] if len(parts) > 2 else "")
        linux_dram = _parse_secs(parts[4] if len(parts) > 4 else "")
        distributed = _parse_secs(parts[5] if len(parts) > 5 else "")
        if mixed is None and cxl is None and linux_dram is None and distributed is None:
            continue
        rows.append({
            "machine": int(parts[0]),
            "mixed_cxl": mixed,
            "cxl": cxl,
            "linux_dram": linux_dram,
            "distributed": distributed,
        })
    if not rows:
        raise SystemExit(f"No gemini data rows in {data_path}")
    df = pd.DataFrame(rows).sort_values("machine")
    missing = [f"{series}/N={n}" for series in
               ["mixed_cxl", "cxl", "linux_dram", "distributed"] for n in X_TICKS
               if df.loc[df["machine"] == n, series].dropna().empty]
    if missing and not allow_partial:
        raise SystemExit("Incomplete Gemini dataset: " + ", ".join(missing))
    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (3.8, 4.16)})
    fig, ax1 = plt.subplots()
    x = df["machine"].values

    def _plot_series(key, label, color, linestyle, marker):
        vals = df[key]
        mask = vals.notna()
        if not mask.any():
            return
        ax1.plot(df.loc[mask, "machine"].values, 100 * (1 / vals[mask].values),
                 label=label, color=color, linestyle=linestyle, marker=marker,
                 markersize=8, markeredgewidth=2, linewidth=2.2)

    _plot_series("mixed_cxl", f"{SYSTEM_NAME} Mixed", "#d62728", "-", "x")
    _plot_series("cxl", f"{SYSTEM_NAME} CXL", "#1f77b4", "-.", "s")
    _plot_series("linux_dram", "Ideal", "silver", "--", "d")
    _plot_series("distributed", "Distributed", "black", "--", "o")
    ax1.set_ylabel("1/Run Time (1/s * 100)")
    ax1.set_xticks(x)
    ax1.set_xlabel("#Machines")
    ax1.grid(True, which="both", axis="both", linestyle=":")
    fig.tight_layout()
    _draw_fig(fig_dir, "gemini-chcore")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv-dir", type=Path, default=SCRIPT_DIR / "csv")
    ap.add_argument("--fig-dir", type=Path, default=SCRIPT_DIR / "figures")
    ap.add_argument("--log-dir", type=Path,
                    help="Sweep logs (<app>_<Mixed|CXL>_N<n>.log); write csv/ then plot")
    ap.add_argument("--matrix-data", type=Path, help="mapreduce RESULT: text")
    ap.add_argument("--db1000-data", type=Path, help="db1000 module,machines,perf CSV")
    ap.add_argument("--gemini-data", type=Path, help="gemini machines,...,DISTRIBUTED CSV/log")
    ap.add_argument("--allow-partial", action="store_true",
                    help="debug only: plot available series/points")
    ap.add_argument("--require-log", action="append", default=[], metavar="BASENAME",
                    help="require this requested sweep log to contain its matching metric; repeatable")
    args = ap.parse_args()

    fig_dir = args.fig_dir
    csv_dir = args.csv_dir
    csv_dir.mkdir(parents=True, exist_ok=True)
    fig_dir.mkdir(parents=True, exist_ok=True)
    if args.require_log and not args.log_dir:
        ap.error("--require-log requires --log-dir")
    if args.log_dir:
        written = collect_from_logs(
            args.log_dir, csv_dir, args.require_log
        )
        args.matrix_data = args.matrix_data or written.get("matrix")
        args.db1000_data = args.db1000_data or written.get("db1000")
        args.gemini_data = args.gemini_data or written.get("gemini")

    drew = False
    if args.matrix_data:
        draw_matrix(args.matrix_data, fig_dir, args.allow_partial); drew = True
    if args.db1000_data:
        draw_db1000(args.db1000_data, fig_dir, args.allow_partial); drew = True
    if args.gemini_data:
        draw_gemini(args.gemini_data, fig_dir, args.allow_partial); drew = True
    if not drew:
        ap.error("provide --log-dir and/or at least one of "
                 "--matrix-data / --db1000-data / --gemini-data")
    draw_legend(fig_dir)


if __name__ == "__main__":
    main()
