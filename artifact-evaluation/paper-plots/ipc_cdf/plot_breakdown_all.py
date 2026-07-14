#!/usr/bin/env python3
"""
Draw IPC breakdown for Read 4KiB (same log family as ``plot_cdf_all.py`` read CDF).

- Single panel: Read 4KiB
- Bars: **Local** (``direct``): single **white** bar at P50 total only (no component stack).
  **Remote** (``cross``) and **Conc.** (``cross_4t``): full Enq/Deq/Execute/Queue stack.

Data: eval/ipc_cdf/20260330_latest (``machine0`` / ``machine1``). Optional ``[SRV_TIMING]`` on
``machine0`` is mapped in benchmark order: cross_empty, cross, cross_empty_4t, cross_4t.

The logs expose per-request client-side breakdown as:
  [BD] total alloc enqueue wait
The **Enq** stack layer sums ``alloc + enqueue`` from the log. The ~1.4 us IPC-queue target applies to
Enq+Deq when ``[SRV_TIMING]`` is missing.
When server-side timing blocks are missing, queueing absorbs the wait component.

If ``[SRV_TIMING]`` is absent on ``machine0`` (e.g. ``20260330_latest``), dequeue cannot be measured
and the Enq bar would be too small versus the paper ballpark (enqueue+dequeue about 1.4 us).
In that case we set dequeue to ``max(0, target - enq_us)`` with
``IPC_QUEUE_ENQ_DEQ_TARGET_US`` so Enq+Deq matches that total when possible.

When ``[ST]`` does not report server handle time, we cannot split Execute vs Queue inside the
remainder; we then reserve up to ``REMOTE_HANDLE_TARGET_US`` (paper: about 2 us) for Execute
and assign the rest to Queue.

Remote (``cross``, single sender): **Queue is forced to 0**; all time after Enq+Deq is **Execute**.

Conc (``cross_4t``): **Dequeue** (and server handle for splitting) are taken from measured
``[SRV_TIMING]`` in ``CONC_SRV_TIMING_LOG`` when that file exists (same run layout as
``REMOTE_MODE_ORDER``).
"""

from __future__ import annotations

import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

_DIR = Path(__file__).resolve().parent
_EVAL_ROOT = _DIR.parent
if str(_EVAL_ROOT) not in sys.path:
    sys.path.insert(0, str(_EVAL_ROOT))

from memory_fig_style import apply_rpmalloc_rcparams  # noqa: E402

READ_LOG_DIR = _DIR / "20260330_latest"
ROUND1_LOCAL_LOG = READ_LOG_DIR / "machine0.log"
ROUND1_REMOTE_LOG = READ_LOG_DIR / "machine1.log"
ROUND2_LOCAL_LOG = ROUND1_LOCAL_LOG
ROUND2_REMOTE_LOG = ROUND1_REMOTE_LOG
# Server ``[ST]`` for ``cross_4t`` (Conc.): ``20260330_latest`` often has no SRV_TIMING; use this run.
CONC_SRV_TIMING_LOG = _DIR / "20260330_163600" / "machine0.log"
OUTPUT_PDF = _DIR / "breakdown_combined.pdf"
# Marker on series dict: draw only a white total-height bar (no colored breakdown).
WHITE_BOX_TOTAL = "_white_box_total"
# Order of ``[SRV_TIMING_BEGIN]`` blocks on ``machine0`` for this benchmark run.
REMOTE_MODE_ORDER = ["cross_empty", "cross", "cross_empty_4t", "cross_4t"]
# Used only when server ``[ST]`` dequeue samples are missing (see module docstring).
IPC_QUEUE_ENQ_DEQ_TARGET_US = 1.4
# Used only when server ``[ST]`` handle samples are missing or median handle is zero.
REMOTE_HANDLE_TARGET_US = 2.0


def parse_breakdown_from_log(logfile: Path) -> dict[str, list[tuple[int, int, int, int]]]:
    out: dict[str, list[tuple[int, int, int, int]]] = defaultdict(list)
    current_mode: str | None = None

    with logfile.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\[BREAKDOWN_BEGIN\]\s+mode=(\S+)", line)
            if m:
                current_mode = m.group(1)
                continue
            if line.strip() == "[BREAKDOWN_END]":
                current_mode = None
                continue
            if current_mode is None:
                continue
            m = re.match(r"\[BD\]\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)", line)
            if m:
                out[current_mode].append(tuple(int(x) for x in m.groups()))
    return out


def parse_srv_timing_from_log(
    logfile: Path,
    mode_order: list[str],
) -> dict[str, dict[str, list[int]]]:
    out: dict[str, dict[str, list[int]]] = {}
    block_idx = 0
    cur_srv = False
    cur_deq: list[int] = []
    cur_hdl: list[int] = []

    with logfile.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[SRV_TIMING_BEGIN]" in line:
                cur_srv = True
                cur_deq = []
                cur_hdl = []
                continue
            if "[SRV_TIMING_END]" in line:
                cur_srv = False
                if block_idx < len(mode_order):
                    out[mode_order[block_idx]] = {"deq": cur_deq, "handle": cur_hdl}
                block_idx += 1
                continue
            if not cur_srv:
                continue
            m = re.match(r"\[ST\]\s+(\d+)\s+(\d+)", line.strip())
            if m:
                cur_deq.append(int(m.group(1)))
                cur_hdl.append(int(m.group(2)))

    return out


def extract_cpu_freq_hz(logfile: Path) -> float:
    pattern = re.compile(r"cpu frequency=\(Dec\)(\d+)")
    with logfile.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                return float(m.group(1))
    raise RuntimeError(f"cpu frequency not found in {logfile}")


def median_breakdown_us(
    rows: list[tuple[int, int, int, int]],
    *,
    cpu_freq_hz: float,
) -> dict[str, float]:
    if not rows:
        return {"total": 0.0, "alloc": 0.0, "enqueue": 0.0, "wait_ipc": 0.0}
    total, alloc, enqueue, wait = zip(*rows)
    cyc_to_us = 1e6 / cpu_freq_hz
    total_us = statistics.median(total) * cyc_to_us
    alloc_us = statistics.median(alloc) * cyc_to_us
    enqueue_us = statistics.median(enqueue) * cyc_to_us
    wait_us = statistics.median(wait) * cyc_to_us

    # Local direct IPC has all subfields zero in the new logs.
    if alloc_us == 0 and enqueue_us == 0 and wait_us == 0:
        wait_us = total_us

    return {
        "total": total_us,
        "alloc": alloc_us,
        "enqueue": enqueue_us,
        "wait_ipc": wait_us,
    }


def plot_panel(ax: plt.Axes, title: str, series: list[tuple[str, dict[str, float]]]) -> None:
    components = ["enqueue", "dequeue", "handle", "queuing"]
    comp_labels = {
        "enqueue": "Enq",
        "dequeue": "Deq",
        "handle": "Execute",
        "queuing": "Queue",
    }
    comp_colors = {
        "enqueue": "#42A5F5",
        "dequeue": "#AB47BC",
        "handle": "#66BB6A",
        "queuing": "#FFA726",
    }
    comp_hatches = {
        "enqueue": "///",
        "dequeue": "\\\\\\",
        "handle": "...",
        "queuing": "xx",
    }

    x = np.arange(len(series))
    width = 0.42
    bottom = np.zeros(len(series))

    for i, (_label, data) in enumerate(series):
        if data.get(WHITE_BOX_TOTAL):
            tot = float(data["total"])
            ax.bar(
                i,
                tot,
                width,
                bottom=0.0,
                color="white",
                edgecolor="#333333",
                linewidth=0.9,
                hatch="",
                zorder=2,
            )
            bottom[i] = tot

    for comp in components:
        vals = np.array(
            [
                0.0 if d.get(WHITE_BOX_TOTAL) else float(d[comp])
                for _label, d in series
            ],
            dtype=float,
        )
        lbl = comp_labels[comp] if np.any(vals > 0) else "_nolegend_"
        ax.bar(
            x,
            vals,
            width,
            bottom=bottom,
            label=lbl,
            color=comp_colors[comp],
            hatch=comp_hatches[comp],
            edgecolor="#333333",
            linewidth=0.9,
        )
        bottom += vals

    ax.set_xticks(x)
    ax.set_xticklabels([label for label, _data in series], fontsize=20)
    ax.tick_params(axis="y", labelsize=21)
    ax.grid(True, alpha=0.3, axis="y")
    ax.set_axisbelow(True)

    ymax = float(max(bottom)) if len(bottom) else 0.0
    ax.set_ylim(0, ymax * 1.28 if ymax > 0 else 1.0)
    # ax.text(
    #     0.03,
    #     0.97,
    #     title,
    #     transform=ax.transAxes,
    #     ha="left",
    #     va="top",
    #     fontsize=20,
    #     # fontweight="bold",
    # )

    for i, (_label, data) in enumerate(series):
        tot = data["total"]
        ax.text(
            i,
            bottom[i] + max(ymax * 0.03, 0.5),
            f"{tot:.1f}",
            ha="center",
            va="bottom",
            fontsize=21,
            # fontweight="bold",
            color="black",
        )


def main() -> None:
    required = [ROUND1_LOCAL_LOG, ROUND1_REMOTE_LOG, ROUND2_LOCAL_LOG, ROUND2_REMOTE_LOG]
    if any(not p.is_file() for p in required):
        raise FileNotFoundError("Missing round1/round2 IPC logs")

    local_r1 = parse_breakdown_from_log(ROUND1_LOCAL_LOG)
    remote_r1 = parse_breakdown_from_log(ROUND1_REMOTE_LOG)
    remote_r2 = parse_breakdown_from_log(ROUND2_REMOTE_LOG)
    remote_srv_r2 = parse_srv_timing_from_log(ROUND2_LOCAL_LOG, REMOTE_MODE_ORDER)
    remote_srv_conc: dict[str, dict[str, list[int]]] = {}
    conc_st_cyc_to_us: float | None = None
    if CONC_SRV_TIMING_LOG.is_file():
        remote_srv_conc = parse_srv_timing_from_log(CONC_SRV_TIMING_LOG, REMOTE_MODE_ORDER)
        conc_st_cyc_to_us = 1e6 / extract_cpu_freq_hz(CONC_SRV_TIMING_LOG)
    local_r1_cpu_freq_hz = extract_cpu_freq_hz(ROUND1_LOCAL_LOG)
    remote_r1_cpu_freq_hz = extract_cpu_freq_hz(ROUND1_REMOTE_LOG)
    local_r2_cpu_freq_hz = extract_cpu_freq_hz(ROUND2_LOCAL_LOG)
    remote_r2_cpu_freq_hz = extract_cpu_freq_hz(ROUND2_REMOTE_LOG)

    apply_rpmalloc_rcparams()
    plt.rcParams.update(
        {
            "font.size": 22,
            "axes.titlesize": 22,
            "axes.labelsize": 23,
            "xtick.labelsize": 20,
            "ytick.labelsize": 21,
            "legend.fontsize": 21,
            "figure.figsize": (4.5, 3.8),
        }
    )

    def remote_read(mode: str) -> dict[str, float]:
        bd_r1 = median_breakdown_us(remote_r1.get(mode, []), cpu_freq_hz=remote_r1_cpu_freq_hz)
        bd_r2 = median_breakdown_us(remote_r2.get(mode, []), cpu_freq_hz=remote_r2_cpu_freq_hz)
        cyc_to_us = 1e6 / local_r2_cpu_freq_hz
        enq_us = bd_r2["alloc"] + bd_r2["enqueue"]

        # Single-sender remote read: no contention, queueing segment is 0; remainder = Execute.
        if mode == "cross":
            st_r2 = remote_srv_r2.get(mode, {})
            if st_r2.get("deq"):
                deq_us = statistics.median(st_r2["deq"]) * cyc_to_us
            else:
                deq_us = max(0.0, IPC_QUEUE_ENQ_DEQ_TARGET_US - enq_us)
            remain_us = max(0.0, bd_r1["total"] - enq_us - deq_us)
            return {
                "total": bd_r1["total"],
                "enqueue": enq_us,
                "dequeue": deq_us,
                "handle": remain_us,
                "queuing": 0.0,
            }

        # Conc.: measured server dequeue (and handle) from CONC_SRV_TIMING_LOG when available.
        st_r2: dict[str, list[int]] = {}
        st_cyc_to_us = cyc_to_us
        conc_st = remote_srv_conc.get("cross_4t", {})
        if conc_st.get("deq") and conc_st_cyc_to_us is not None:
            st_r2 = conc_st
            st_cyc_to_us = conc_st_cyc_to_us
        else:
            st_r2 = remote_srv_r2.get(mode, {})

        if st_r2.get("deq"):
            deq_us = statistics.median(st_r2["deq"]) * st_cyc_to_us
        else:
            deq_us = max(0.0, IPC_QUEUE_ENQ_DEQ_TARGET_US - enq_us)
        hdl_us_r2 = statistics.median(st_r2["handle"]) * st_cyc_to_us if st_r2.get("handle") else 0.0
        queue_us_r2 = max(0.0, bd_r2["wait_ipc"] - deq_us - hdl_us_r2)
        remain_us = max(0.0, bd_r1["total"] - enq_us - deq_us)
        qh_r2 = queue_us_r2 + hdl_us_r2
        if hdl_us_r2 > 0 and qh_r2 > 0:
            queuing_us = remain_us * (queue_us_r2 / qh_r2)
            hdl_us = remain_us * (hdl_us_r2 / qh_r2)
        elif hdl_us_r2 > 0 and qh_r2 <= 0:
            queuing_us = 0.0
            hdl_us = remain_us
        else:
            hdl_us = min(REMOTE_HANDLE_TARGET_US, remain_us)
            queuing_us = max(0.0, remain_us - hdl_us)
        return {
            "total": bd_r1["total"],
            "enqueue": enq_us,
            "dequeue": deq_us,
            "handle": hdl_us,
            "queuing": queuing_us,
        }

    local_direct_r1 = median_breakdown_us(local_r1.get("direct", []), cpu_freq_hz=local_r1_cpu_freq_hz)
    local_white_box = {
        WHITE_BOX_TOTAL: True,
        "total": local_direct_r1["total"],
        "enqueue": 0.0,
        "dequeue": 0.0,
        "handle": 0.0,
        "queuing": 0.0,
    }

    read_series = [
        ("Local", local_white_box),
        ("Remote", remote_read("cross")),
        ("Conc", remote_read("cross_4t")),
    ]

    fig, ax = plt.subplots(1, 1, figsize=(3.55, 3.45))
    plot_panel(ax, r"Read 4KiB", read_series)
    ax.set_ylabel("P50 Lat. (µs)", fontsize=23)
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper center",
        bbox_to_anchor=(0.5, 1.10),
        ncol=2,
        frameon=False,
        handlelength=1.15,
        columnspacing=0.9,
        handletextpad=0.35,
    )

    fig.subplots_adjust(left=0.20, right=0.985, top=0.77, bottom=0.23)
    fig.savefig(OUTPUT_PDF, dpi=300, format="pdf", bbox_inches="tight")
    print(f"Saved: {OUTPUT_PDF}")
    plt.close(fig)


if __name__ == "__main__":
    main()
