#!/usr/bin/env python3
import argparse
import csv
import re
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_OUT = SCRIPT_DIR
SAMPLE_RE = re.compile(
    r"\[MSI_LATENCY_BENCH\] metric=delivery sample=(\d+) latency_ns=(\d+)"
)


def nearest_rank(values, percentile):
    ordered = sorted(values)
    rank = max(1, (len(ordered) * percentile + 99) // 100)
    return ordered[int(rank) - 1]


def main():
    parser = argparse.ArgumentParser(
        description="Parse the one-way ivshmem MSI delivery benchmark"
    )
    parser.add_argument("--log-dir", type=Path, default=SCRIPT_DIR / "logs")
    parser.add_argument("--csv-dir", type=Path, default=SCRIPT_DIR / "csv")
    args = parser.parse_args()

    rows = []
    for run_dir in sorted(args.log_dir.glob("run*")):
        try:
            run = int(run_dir.name[3:])
        except ValueError:
            continue
        log = run_dir / "machine0.log"
        if not log.exists():
            continue
        for match in SAMPLE_RE.finditer(log.read_text(errors="replace")):
            rows.append((run, int(match.group(1)), int(match.group(2))))

    if not rows:
        parser.error(f"no MSI samples found under {args.log_dir}")

    args.csv_dir.mkdir(parents=True, exist_ok=True)
    with (args.csv_dir / "msi_samples.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("run", "sample", "latency_ns"))
        writer.writerows(rows)

    values = [row[2] for row in rows]
    summary = {
        "runs": len({row[0] for row in rows}),
        "samples": len(values),
        "avg_ns": sum(values) / len(values),
        "min_ns": min(values),
        "median_ns": nearest_rank(values, 50),
        "p95_ns": nearest_rank(values, 95),
        "p99_ns": nearest_rank(values, 99),
        "max_ns": max(values),
    }
    with (args.csv_dir / "msi_summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=summary.keys())
        writer.writeheader()
        writer.writerow(summary)

    print(
        "MSI delivery: "
        f"samples={summary['samples']} avg_us={summary['avg_ns'] / 1000:.3f} "
        f"median_us={summary['median_ns'] / 1000:.3f} "
        f"p95_us={summary['p95_ns'] / 1000:.3f} "
        f"p99_us={summary['p99_ns'] / 1000:.3f}"
    )


if __name__ == "__main__":
    main()
