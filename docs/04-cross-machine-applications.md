# 4. Cross-machine applications

Starfish partitions application state by whether it must move or be shared; it does not place an entire process in CXL memory.

## Migration and placement

The DSM process flow prepares the process, coordinates affected connections, represents transferable objects in shared form, restores/promotes target-side threads, and reconnects required services.

[kernel/dsm/dsm_migrate.c](../kernel/dsm/dsm_migrate.c) implements `dsm_migrate_process_prepare`, `dsm_migrate_process_ckpt`, and `dsm_migrate_process_restore`. `kernel/dsm/dsm_objects/` holds object-specific rules for capability groups, connections, threads, vmspaces, PMOs, pages, notifications, and IRQ objects. Scheduler handoff is in `kernel/sched/policy_rr.c`.

Read `kernel/dsm_config.cmake` with the active `.config`: user allocation is distinct from the placement of thread contexts, page tables, stacks, kernel objects, and pages. [kernel/mm/pgfault_handler.c](../kernel/mm/pgfault_handler.c) handles migration races; [kernel/dsm/dsm_tiering.c](../kernel/dsm/dsm_tiering.c) selects promotion/demotion behavior.

## Workloads

Paper-oriented ports under `user/demos/` include:

- `phoenix-2.0/`: MapReduce;
- `dbx1000/`: OLTP;
- `GeminiGraph` and `GeminiGraph-update/`: graph analytics;
- `VeryTinyCnn/`: inference;
- `leveldb-1.23/`, `redis-6.0.8/`, `memcached/`: key-value stores.

Not every port is enabled in the checked-in `.config`. Use `./chbuild config` or edit the configuration, then rebuild. Driver scripts in `dsm-scripts/tests/` and `artifact-evaluation/` define supported benchmark contracts.

A failed machine's process is not transparently resumed. Surviving applications/services continue when their dependencies remain available; failed processes restart or use their own durable state.

