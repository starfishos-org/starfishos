# 9 — Resource utilization / co-location (paper Fig real)

**Status: stub** — entry script not implemented yet.

Target paper figure (`p3os-paper/eval/`):

- `real.eps`

## Intended workflow

1. Enable required demos in `user/demos/config.cmake`.
2. Drive the co-location / resource-utilization suite under
   `dsm-scripts/tests/real/`.
3. Emit CSV matching `p3os-paper/eval/real.csv`.
4. Plot with `p3os-paper/eval/real.py`.

## Run (once implemented)

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/9-resource-util/run.sh
```

Until then, `run-all.sh` reports this experiment as `STUB(not implemented)`.
