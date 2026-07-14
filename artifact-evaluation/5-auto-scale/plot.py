#!/usr/bin/env python3
"""Parse auto-scaling sweep logs and plot the three paper auto-scale figures:

  auto-scale-matrix.eps  — Matrix-multiply MapReduce, throughput vs #machines
  db1000.eps             — DBx1000 TPC-C throughput vs #machines
  gemini-chcore.eps      — GeminiGraph PageRank runtime->throughput vs #machines

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

  python3 plot.py --out-dir /tmp/as-check \
    --matrix-data /mnt/disk1/yjs/p3os-paper/eval/mapreduce/4000size.txt \
    --db1000-data /mnt/disk1/yjs/p3os-paper/eval/db1000/db1000-p3os-tigon.csv \
    --gemini-data /mnt/disk1/yjs/p3os-paper/eval/gemini_graph/data.log

Collect StarfishOS curves from a sweep then plot::

  python3 plot.py --out-dir out/<ts> --log-dir out/<ts>/logs
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.ticker import MultipleLocator

SYSTEM_NAME = "Starfish"
X_TICKS = [1, 2, 4, 6, 8]

LOG_NAME_RE = re.compile(
    r"^(?P<app>matrix|db1000|gemini)_(?P<config>Mixed|CXL)_N(?P<n>\d+)\.log$"
)
PAT_PHOENIX_US = re.compile(r"(?<!inter )library:\s*([\d.]+)")
PAT_FINALIZE_US = re.compile(r"finalize:\s*([\d.]+)")
PAT_THP = re.compile(r"thp=([\d.eE+-]+)")
PAT_EXEC_TIME = re.compile(r"exec_time[:=]\s*([\d.]+)")

CONFIG_TO_MATRIX = {"Mixed": "MIXED", "CXL": "CXL"}
CONFIG_TO_DB1000 = {"Mixed": "P3OS-mixed", "CXL": "P3OS-all_cxl"}


def _draw_fig(fig_dir: Path, name: str):
    fig_dir.mkdir(parents=True, exist_ok=True)
    stem = fig_dir / name
    plt.savefig(stem.with_suffix(".eps"), format="eps", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".pdf"), format="pdf", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".png"), dpi=200, format="png", bbox_inches="tight")
    print(f"Wrote {stem}.eps/.pdf/.png")


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


def collect_from_logs(log_dir: Path, results_dir: Path) -> Dict[str, Path]:
    """Convert run.sh sweep logs into the three plotter data files.

    Returns a dict of logical name -> written path for files that were produced.
    """
    results_dir.mkdir(parents=True, exist_ok=True)

    matrix_lines: List[str] = []
    db1000_rows: List[Tuple[str, int, float]] = []
    # machine -> {Mixed: secs, CXL: secs}
    gemini: Dict[int, Dict[str, float]] = {}

    for path in sorted(log_dir.glob("*.log")):
        m = LOG_NAME_RE.match(path.name)
        if not m:
            continue
        app, config, n = m.group("app"), m.group("config"), int(m.group("n"))
        text = path.read_text(errors="replace")
        if app == "matrix":
            us = _extract_matrix_us(text)
            if us is None:
                print(f"[WARN] no matrix timing in {path.name}")
                continue
            matrix_lines.append(
                f"RESULT: N={n} CONFIG={CONFIG_TO_MATRIX[config]} TIME={int(us)}"
            )
        elif app == "db1000":
            thp = _extract_db1000_thp(text)
            if thp is None:
                print(f"[WARN] no thp= in {path.name}")
                continue
            db1000_rows.append((CONFIG_TO_DB1000[config], n, thp))
        elif app == "gemini":
            secs = _extract_gemini_secs(text)
            if secs is None:
                print(f"[WARN] no exec_time in {path.name}")
                continue
            gemini.setdefault(n, {})[config] = secs

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
            mixed = gemini[n].get("Mixed")
            cxl = gemini[n].get("CXL")
            mixed_s = f"{mixed}s" if mixed is not None else ""
            cxl_s = f"{cxl}s" if cxl is not None else ""
            # External baselines are filled in separately; leave empty here.
            lines.append(f"{n},{mixed_s},{cxl_s},,,")
        out.write_text("\n".join(lines) + "\n")
        print(f"Wrote {out} ({len(gemini)} machine counts)")
        written["gemini"] = out

    return written


# ── auto-scale-matrix ───────────────────────────────────────────────────────

def draw_matrix(data_path: Path, fig_dir: Path):
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

def draw_db1000(data_path: Path, fig_dir: Path):
    data = pd.read_csv(data_path).sort_values(["module", "machines"])
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


def draw_gemini(data_path: Path, fig_dir: Path):
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
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--log-dir", type=Path,
                    help="Sweep logs (<app>_<Mixed|CXL>_N<n>.log); write results/ then plot")
    ap.add_argument("--matrix-data", type=Path, help="mapreduce RESULT: text")
    ap.add_argument("--db1000-data", type=Path, help="db1000 module,machines,perf CSV")
    ap.add_argument("--gemini-data", type=Path, help="gemini machines,...,DISTRIBUTED CSV/log")
    args = ap.parse_args()

    fig_dir = args.out_dir / "figures"
    if args.log_dir:
        written = collect_from_logs(args.log_dir, args.out_dir / "results")
        args.matrix_data = args.matrix_data or written.get("matrix")
        args.db1000_data = args.db1000_data or written.get("db1000")
        args.gemini_data = args.gemini_data or written.get("gemini")

    drew = False
    if args.matrix_data:
        draw_matrix(args.matrix_data, fig_dir); drew = True
    if args.db1000_data:
        draw_db1000(args.db1000_data, fig_dir); drew = True
    if args.gemini_data:
        draw_gemini(args.gemini_data, fig_dir); drew = True
    if not drew:
        ap.error("provide --log-dir and/or at least one of "
                 "--matrix-data / --db1000-data / --gemini-data")


if __name__ == "__main__":
    main()
