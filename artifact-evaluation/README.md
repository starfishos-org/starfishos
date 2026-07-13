# Artifact evaluation

Run the global environment preparation before running individual artifact
tests:

```bash
./artifact-evaluation/prepare.sh
```

The global preparation creates and initializes:

- CXL shared-memory backing file
- 8 NUMA backing files, serving as local DRAM of 8 machines
- hostfs shared-memory backing file and metadata
- ivshmem doorbell server

The preparation script is idempotent by default: it creates missing backing
files and refreshes metadata, but it does not rewrite existing large files. To
force a full rebuild:

```bash
./artifact-evaluation/prepare.sh recreate
```

After that, run individual tests from their subdirectories, for example:

```bash
./artifact-evaluation/ipc-cdf/run.sh
```

Each test script checks that the global environment is present. It does not
recreate the large backing files.
