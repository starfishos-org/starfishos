#!/usr/bin/env python3
"""Summarize CXL CLOCK aging counters from kernel logs.

Parses the [CXL_CLOCK], [CXL_AGING], [CXL_ELIGIBLE] and [CXL_ADMISSION] lines
emitted by kernel/dsm/dsm_cxl_reclaim.c and reports how the second-chance
policy classified the pages it aged -- in particular the re-referenced share,
which says whether access recency carries enough signal to be worth encoding
in the queue order.

The counters live in dsm_meta, which sits in shared CXL memory, so every
machine observes the same cluster-wide totals.  Values are therefore reduced
with max() across logs, never summed.

Usage:
    parse_cxl_aging_stats.py <log-or-dir> [<log-or-dir> ...]
"""

import argparse
import re
import sys
from pathlib import Path

# One regex per kinfo() line; each captures the "key=value" tail for a
# generic split, so adding a counter kernel-side needs no change here.
LINE_RES = {
    "init": re.compile(r"\[CXL_RECLAIM\] initialized\s+(.*)"),
    "clock": re.compile(r"\[CXL_CLOCK\]\s+(.*)"),
    "aging": re.compile(r"\[CXL_AGING\]\s+(.*)"),
    "eligible": re.compile(r"\[CXL_ELIGIBLE\]\s+(.*)"),
    "admission": re.compile(r"\[CXL_ADMISSION\]\s+(.*)"),
    "sample": re.compile(r"\[CXL_SAMPLE\]\s+(.*)"),
}
KV_RE = re.compile(r"([a-z_0-9]+)=(\d+)")

# Fields that only ever increase.  These are cluster-wide totals in dsm_meta,
# so the largest value seen across all logs is the latest value.
COUNTER_KEYS = {
    "scans", "second_chances", "armed", "hot", "one_epoch_cold",
    "stable_cold", "cold_evictions", "pressure_evictions", "cooldown_skips",
    "scan_skips", "batches", "pages", "machines", "total_latency_ns",
    "max_latency_ns", "soft_limit_overcommits", "async_requests",
    "scheduled_fallbacks", "promotions", "refaults", "conflicts",
    "reclaimed", "rotations",
}
# Everything else is a gauge: it rises and falls, so max() would report a
# high-water mark while labelling it as the current state.  Resident-page
# counts, per-PMO eligibility, the in-progress sample position and the
# per-round latency sample are all of this kind.  Gauges are taken from the
# most recent line instead, ordered by the monotonic counter on that line
# (or, for lines that carry none, the last one seen before them in the file).
ANCHOR_KEYS = {"clock": "scans", "aging": "batches", "admission": "promotions"}


def parse_file(path, totals, gauges, seen):
    """Fold one log into `totals` (counters) and `gauges` (latest-wins)."""
    try:
        text = path.read_text(errors="replace")
    except OSError as exc:
        print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
        return

    anchor = 0
    for line in text.splitlines():
        for kind, line_re in LINE_RES.items():
            match = line_re.search(line)
            if not match:
                continue
            seen[kind] = seen.get(kind, 0) + 1
            fields = {k: int(v) for k, v in KV_RE.findall(match.group(1))}

            anchor_key = ANCHOR_KEYS.get(kind)
            if anchor_key and anchor_key in fields:
                anchor = max(anchor, fields[anchor_key])

            counters = totals.setdefault(kind, {})
            for key, value in fields.items():
                if key in COUNTER_KEYS:
                    counters[key] = max(counters.get(key, 0), value)

            gauge_fields = {k: v for k, v in fields.items()
                            if k not in COUNTER_KEYS}
            if gauge_fields:
                previous = gauges.get(kind)
                if previous is None or anchor >= previous[0]:
                    gauges[kind] = (anchor, gauge_fields)


def collect(paths):
    totals, gauges, seen = {}, {}, {}
    files = []
    for raw in paths:
        p = Path(raw)
        if p.is_dir():
            files.extend(sorted(p.glob("**/exec_log*.log")))
        else:
            files.append(p)
    if not files:
        print("no log files found", file=sys.stderr)
        sys.exit(1)
    for path in files:
        parse_file(path, totals, gauges, seen)
    return totals, gauges, seen, files


def pct(part, whole):
    return f"{100.0 * part / whole:6.2f}%" if whole else "     --"


def gauge(gauges, kind, key, default=0):
    """Latest observed value of a rising-and-falling field."""
    entry = gauges.get(kind)
    return entry[1].get(key, default) if entry else default


def report(totals, gauges, seen, files):
    print(f"parsed {len(files)} log file(s); "
          + ", ".join(f"{k}={v}" for k, v in sorted(seen.items()))
          + " matching lines")

    clock = totals.get("clock", {})
    aging = totals.get("aging", {})

    if not aging:
        print("\nNo [CXL_AGING] lines: the demoter never completed an aging "
              "epoch.\nCXL residency probably stayed under the soft limit, so "
              "no reclaim was requested.")
        if gauges.get("admission"):
            print(f"  resident={gauge(gauges, 'admission', 'resident')} "
                  f"reserved={gauge(gauges, 'admission', 'reserved')} "
                  f"limit={gauge(gauges, 'admission', 'limit')} pages")
        return

    aged = aging.get("pages", 0)
    armed = clock.get("armed", 0)
    hot = clock.get("hot", 0)
    one_cold = clock.get("one_epoch_cold", 0)
    stable = clock.get("stable_cold", 0)
    classified = armed + hot + one_cold + stable

    print("\n=== aging throughput ===")
    print(f"  pages aged (epoch observations) : {aged}")
    print(f"  shootdown batches               : {aging.get('batches', 0)}")
    print(f"  machine-touches                 : {aging.get('machines', 0)}")
    # record_aging_stats() adds one latency sample per *round* but adds the
    # round's batch count to `batches`, so total/batches is not a per-batch
    # mean.  There is no round counter in dsm_meta; a round ages at most
    # async_batch pages, which makes pages/async_batch a good round estimate
    # (it agrees with batches/machines-per-round on every run seen so far).
    total_ns = aging.get("total_latency_ns", 0)
    per_round = gauge(gauges, "init", "async_batch")
    rounds = aged / per_round if per_round else 0
    print(f"  total aging shootdown time      : {total_ns / 1e9:.1f} s "
          f"(aggregate across machines)")
    if rounds:
        print(f"  estimated aging rounds          : {rounds:.0f}")
        print(f"  mean latency per round          : {total_ns / rounds / 1000:.0f} us")
    print(f"  max single-round latency        : {aging.get('max_latency_ns', 0) / 1e6:.1f} ms")

    print("\n=== observation mix ===")
    # `armed` is the baseline epoch (A-bit cleared, nothing concluded yet);
    # the re-referenced share is only meaningful against concluded epochs.
    concluded = hot + one_cold + stable
    print(f"  ARMED (baseline epoch)     : {armed:8d}  {pct(armed, classified)} of classified")
    print(f"  REREFERENCED (hot)         : {hot:8d}  {pct(hot, concluded)} of concluded")
    print(f"  ONE_EPOCH_COLD             : {one_cold:8d}  {pct(one_cold, concluded)} of concluded")
    print(f"  STABLE_COLD (newly)        : {stable:8d}  {pct(stable, concluded)} of concluded")
    print(f"  classified total           : {classified:8d}")
    if aged > classified:
        # [CXL_CLOCK] is emitted by the rate-limited report_clock_stats(),
        # which only fires on a second chance or an eviction, while
        # [CXL_AGING] is emitted every CXL_AGING_REPORT_INTERVAL batches.  In
        # a run with few of either, the clock snapshot is simply older than
        # the aging snapshot, so the two families are not directly comparable.
        print(f"  NOTE: {aged - classified} more pages were aged than the "
              f"clock snapshot covers\n"
              f"        ([CXL_CLOCK] sampled at scans={clock.get('scans', 0)}, "
              f"[CXL_AGING] at pages={aged});\n"
              f"        the mix above is a snapshot, not the whole run.")

    print("\n=== outcome ===")
    print(f"  cold evictions             : {clock.get('cold_evictions', 0)}")
    print(f"  cooldown skips             : {clock.get('cooldown_skips', 0)}")
    print(f"  scan skips                 : {clock.get('scan_skips', 0)}")
    print(f"  clock scans (entries)      : {clock.get('scans', 0)}")

    eligible = gauges.get("eligible", (0, {}))[1]
    if eligible:
        print("\n=== eligible pages by PMO type / perm (latest sample) ===")
        for key, value in sorted(eligible.items()):
            if value:
                print(f"  {key:28s}: {value}")

    admission = totals.get("admission", {})
    if admission or gauges.get("admission"):
        print("\n=== admission ===")
        print(f"  resident={gauge(gauges, 'admission', 'resident')} "
              f"reserved={gauge(gauges, 'admission', 'reserved')} "
              f"limit={gauge(gauges, 'admission', 'limit')} pages "
              f"(latest, not peak)")
        print(f"  promotions={admission.get('promotions', 0)} "
              f"refaults={admission.get('refaults', 0)} "
              f"reclaimed={admission.get('reclaimed', 0)} "
              f"conflicts={admission.get('conflicts', 0)}")

    print("\n=== verdict ===")
    if classified:
        # Share of *all* observations is the honest denominator.  Quoting hot
        # against "concluded" alone reads as 100% whenever no page has managed
        # a cold epoch yet, which is exactly the degenerate case below.
        print(f"  re-referenced share of all observations: "
              f"{100.0 * hot / classified:.2f}%  (n={hot})")
    armed_share = 100.0 * armed / classified if classified else 0.0
    if armed_share > 90.0:
        print(f"  {armed_share:.1f}% of observations are ARMED baselines, and "
              f"only {one_cold + stable} ever completed a cold epoch.")
        print("  -> The hand is not revisiting pages often enough to conclude "
              "anything.  The re-referenced share is not a usable signal at "
              "this sample size; fix the revisit rate first.")
    elif hot and classified and 100.0 * hot / classified < 5.0:
        print("  -> Access recency carries little signal at this epoch "
              "length; ordering the queue by it would change few decisions.")
    elif hot and classified and 100.0 * hot / classified > 30.0:
        print("  -> A large fraction of aged pages are still hot; queue "
              "order that encodes recency could avoid re-aging them.")

    # How long before a page is looked at again?  That period, not the
    # classification mix, is what decides whether the Accessed bit can still
    # discriminate: over a window longer than the workload's reuse distance
    # every live page comes back "referenced".
    #
    # The hand advances by however many entries select_candidates() inspects,
    # and that loop stops at `count < limit` -- i.e. after async_batch
    # candidates -- normally long before it reaches scan_limit.  With few
    # skips (scan_skips ~ 0) the sweep rate is therefore async_batch per scan
    # interval, not scan_limit per scan interval.
    resident = gauge(gauges, "admission", "resident")
    window = gauge(gauges, "sample", "window_pages")
    passes = gauge(gauges, "sample", "passes")
    async_batch = gauge(gauges, "init", "async_batch")
    scan_ns = gauge(gauges, "init", "scan_interval_ns")
    if async_batch and scan_ns:
        per_sec = async_batch / (scan_ns / 1e9)
        if window:
            # Sampling build: the hand re-walks a bounded window, so a page is
            # revisited once per pass over that window -- independent of how
            # big the resident set is.  Using the resident set here (as the
            # pre-sampling formula did) overstates the period by resident/window.
            revisit_s = window / per_sec
            print(f"\n  Sampling window: {window} pages at {per_sec:.0f} "
                  f"entries/s = {revisit_s:.2f} s per revisit")
            if passes:
                print(f"  {passes} passes per window, so a page concludes in "
                      f"~{passes * revisit_s:.1f} s.")
            if resident:
                cover_s = (resident / window) * (passes or 1) * revisit_s
                print(f"  Covering all {resident} resident pages once takes "
                      f"~{cover_s / 60:.1f} min ({gauge(gauges, 'sample', 'rotations') or totals.get('sample', {}).get('rotations', 0)} rotations so far).")
        elif resident:
            revisit_s = resident / per_sec
            print(f"\n  CLOCK revolution: {resident} resident pages at "
                  f"{per_sec:.0f} entries/s = {revisit_s / 60:.1f} min per "
                  f"revisit")
            print(f"  A page needs >=3 observations to become demotable: "
                  f"~{3 * revisit_s / 60:.1f} min minimum before any eviction.")
    rate_batch = gauge(gauges, "init", "rate_batch")
    demote_ns = gauge(gauges, "init", "demote_interval_ns")
    if rate_batch and demote_ns:
        drain = rate_batch / (demote_ns / 1e9)
        print(f"  Demote ceiling: {drain:.0f} pages/s.")
        over = resident - gauge(gauges, "admission", "limit")
        if over > 0:
            print(f"  Draining the current {over} page overcommit at that "
                  f"ceiling would take {over / drain / 3600:.1f} h.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+",
                        help="exec_log files, or directories to scan for them")
    args = parser.parse_args()
    report(*collect(args.paths))


if __name__ == "__main__":
    main()
