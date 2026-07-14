# 8 — Process migration breakdown (paper Fig process-migration)

**Status: stub** — entry script not implemented yet.

Target paper figures (`p3os-paper/eval/`):

- `process-migration-data-large.eps`
- `process-migration-data-small.eps`

## Intended workflow

1. Confirm whether TreeSLS / SSI-SLS / cfork paths are enabled in this tree
   (`kernel/sls_config.cmake`, `dsm-scripts/tests/cfork_*.exp`, `ckpt_*.exp`).
2. Run prepare / checkpoint / restore microbenchmarks across working-set sizes.
3. Emit CSV matching `p3os-paper/eval/process-migration.csv`.
4. Plot with `p3os-paper/eval/process_migration.py`.

## Run (once implemented)

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/8-process-migration/run.sh
```

Until then, `run-all.sh` reports this experiment as `STUB(not implemented)`.
