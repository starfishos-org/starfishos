# Basic platform measurements

Low-level measurements that separate platform cost from higher-level ChCore
paths. Not a numbered paper figure.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/0-basic/run.sh
```

MSI only:

```bash
./artifact-evaluation/0-basic/run_msi.sh
```

## Outputs

Each run creates `artifact-evaluation/0-basic/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/runN/` | MSI: `machine0.log`, `machine1.log` per repetition |
| `logs/` | MLC: `mlc_bandwidth_matrix.log`, `mlc_peak_bandwidth.log` |
| `csv/` | `msi_samples.csv`, `msi_summary.csv` |

## Re-plot MSI

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 0
```

Or point `parse_msi.py` at a specific run:

```bash
python3 artifact-evaluation/0-basic/parse_msi.py \
  --log-dir artifact-evaluation/0-basic/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/0-basic/out/<timestamp>/csv
```

## Intel MLC bandwidth (paper Table 1)

Host-side DRAM/CXL bandwidth for the evaluation setup table.

```bash
./artifact-evaluation/0-basic/run_mlc.sh
# or: python3 artifact-evaluation/run_all.py --run-subset-of-tests 0
```

Set `MLC_BIN` if `mlc` is not on `PATH`. Missing MLC is skipped by default
(`ALLOW_MLC_SKIP=1`).

## Env knobs

MSI: `SAMPLES`, `TARGET_MACHINE`, `TARGET_CPU`, `NRUNS`, `SKIP_BUILD`, `OUT_DIR`,
`LOG_DIR`, `CSV_DIR`, `TS`.

MLC: `MODE` (`matrix`, `peak`, `all`), `MLC_BIN`, `ALLOW_MLC_SKIP`, `LOG_DIR`,
`OUT_DIR`.
