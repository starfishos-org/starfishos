# 6. Machine lifecycle and boot-time rejoin

Starfish assigns each machine a stable logical `machine_id`. If a QEMU guest
crashes, a replacement guest can boot with the same ID and rejoin the existing
cluster without resetting the shared CXL state. The replacement reconstructs
its volatile kernel state in DRAM and attaches to the cluster-wide structures
that survived in CXL memory.

The same generation change is also the failure signal for cross-machine tasks.
When the task's creator remains alive, Starfish terminates the complete task
and reuses the normal process recycler to release its threads, capabilities,
IPC objects, pages, queue nodes, and CXL allocations.

## Persistent lifecycle record

`dsm_metadata_t` is stored at the beginning of the shared CXL region. It
contains one lifecycle record for every supported logical machine:

```c
struct dsm_machine_lifecycle {
    volatile u64 boot_generation;
    volatile u32 state;
    u32 _reserved;
};
```

The records are indexed by `machine_id`:

```c
struct dsm_machine_lifecycle
    machine_lifecycle[CLUSTER_MAX_MACHINE_NUM];
```

Because the records reside in `dsm_metadata_t`, they survive destruction and
replacement of an individual QEMU guest as long as the CXL backing file is not
reset.

### Lifecycle states

| State | Meaning |
| --- | --- |
| `DSM_MACHINE_OFFLINE` | The slot has not joined since the last cold-cluster reset. |
| `DSM_MACHINE_JOINING` | A new logical slot is performing its first boot in this cluster. |
| `DSM_MACHINE_REJOINING` | An existing logical slot is rebuilding after a machine failure. |
| `DSM_MACHINE_ONLINE` | Kernel initialization and creation of the initial user thread have completed. |

The current crash model cannot execute a final write from the failed machine,
so a crash does not change `ONLINE` to `OFFLINE`. `OFFLINE` is currently an
initial/reset state, not a failure detector. A future heartbeat or external
monitor can use the same record to publish confirmed failure.

## `boot_generation`

`boot_generation` identifies an incarnation of a logical machine. It starts at
zero after a full cluster reset and is atomically incremented whenever that
machine ID joins:

```text
machine 0, first boot       -> (machine_id=0, generation=1)
machine 1, first boot       -> (machine_id=1, generation=1)
machine 0, replacement boot -> (machine_id=0, generation=2)
machine 0, next replacement -> (machine_id=0, generation=3)
```

The pair `(machine_id, boot_generation)`, rather than `machine_id` alone,
uniquely identifies a running machine incarnation. This distinction is needed
for future failure recovery: work or references published by machine 0 at
generation 1 must not be mistaken for work produced by the replacement at
generation 2.

The increment uses acquire-release ordering, state publication uses release
stores, and readers use acquire loads through
`dsm_machine_boot_generation()`. The generation is an identity marker, not a
readiness marker: it is published near the beginning of boot. A consumer that
needs a fully reconstructed machine must first acquire-load and observe
`DSM_MACHINE_ONLINE`; only that publication is made after the volatile boot
path and initial user thread are ready.

## First join versus rejoin

The boot argument supplied by `build/simulate.sh` determines the stable
`machine_id`. During `dsm_add_machine()`, the kernel compares that ID with the
persistent `cluster_machine_num`:

```text
machine_id == cluster_machine_num  -> first join; allocate the next slot
machine_id <  cluster_machine_num  -> rejoin an existing slot
machine_id >  cluster_machine_num  -> invalid; machine IDs must join in order
```

This rule is intentionally independent of the lifecycle `state`: a crashed
guest normally leaves its last state as `ONLINE`, but its ID is still below
the already-published cluster size.

For a first join, the kernel extends `cluster_machine_num` and
`cluster_cpu_num`. For a rejoin, neither count changes. The replacement uses
the same global CPU range as the prior incarnation:

```text
cpu_range_low  = machine_id * PLAT_CPU_NUM
cpu_range_high = cpu_range_low + PLAT_CPU_NUM - 1
```

## Cold-cluster boot

A cold boot begins after `make clean-dsm-meta` (normally issued by the AE
launcher). Machine 0 performs the one-time shared initialization:

1. Clear all lifecycle records and initialize all peer-ID mappings as invalid.
2. Mark the durable thread-queue pool as requiring initialization.
3. Publish machine 0 as `JOINING`, generation 1, and add its CPU range.
4. Initialize the shared CXL buddy allocator and publish
   `DSM_CONFIG_STATE_MM_INITED`.
5. Initialize the cluster-wide durable scheduler queues once.
6. Later machines join in increasing ID order, wait for the CXL allocator and
   durable queues, and attach to them.
7. After each machine has rebuilt its kernel and created its initial user
   thread, it publishes `ONLINE`.

Resetting DSM metadata is valid only when no member of the old cluster remains
alive. Doing it during partial-failure recovery would destroy the surviving
cluster's coordination state.

## Rejoin sequence

The rejoin path deliberately keeps the ivshmem server and CXL backing file
alive:

```text
host                    replacement machine 0              surviving machine 1
 | kill old QEMU                  |                                  |
 |--------------------------------X                                  |
 | start QEMU with machine_id=0   |                                  |
 |------------------------------->| map existing CXL                 |
 |                                | detect ID 0 < cluster size       |
 |                                | generation 1 -> 2                |
 |                                | state = REJOINING                 |
 |                                | rebuild DRAM/BSS state            |
 |                                | attach CXL allocator/queues       |
 |                                | republish ivshmem peer ID         |
 |                                | create root thread                |
 |                                | state = ONLINE                    |
 |                                |                                  | remains online
```

The important implementation boundaries are:

- `dsm_add_machine()` does not increase cluster/CPU counts for a rejoin.
- A rejoining machine 0 does not invalidate or rebuild the cluster-wide
  durable scheduler queue pool.
- `ext_mm_init()` observes `DSM_CONFIG_STATE_MM_INITED` and attaches to the
  existing CXL buddy metadata instead of initializing the shared pool again.
- The PCI/ivshmem initialization path overwrites
  `machine_to_peer_id[machine_id]` with the replacement guest's new ivshmem
  peer ID. A peer ID is transport-specific and is not a stable machine ID.
- Normal kernel globals, per-CPU scheduler state, local run queues, locks,
  allocator front ends, and the root process are rebuilt by the replacement
  kernel. They are never restored by following stale DRAM pointers from CXL.
- `dsm_mark_machine_online()` is called only after `create_root_thread()`, so
  `ONLINE` means the replacement has completed the volatile boot path.

## Required invariants

1. A replacement must use the same `machine_id` as the failed guest.
2. The replacement and survivors must map the same CXL backing store.
3. The ivshmem server and CXL backing must not be reset between failure and
   rejoin.
4. A rejoin must not change `cluster_machine_num` or `cluster_cpu_num`.
5. Shared allocator and durable-queue initialization is a cold-boot-only
   operation.
6. Cross-machine work must be tagged with `(machine_id, boot_generation)`
   before a replacement can act on it.

The current implementation assumes the old QEMU is confirmed dead before the
replacement starts. It does not prevent two live guests from using the same
logical ID; fencing or leases are required before supporting that scenario.

## Current limitations

- Lifecycle state is not a heartbeat or failure detector. A crashed machine
  can remain recorded as `ONLINE` until another component confirms failure.
- The old ivshmem peer mapping remains stale between the crash and replacement
  registration. Callers do not yet gate every remote operation on lifecycle
  state.
- Lifecycle rejoin preserves allocator metadata; it does not by itself repair
  every lock or allocator mutation interrupted by a crash. The optional slab
  recovery log and the DSM reference-recovery code are separate mechanisms.
- The basic rejoin regression kills an otherwise idle machine. The separate
  cross-task regression covers a running remote worker and task reclamation,
  but neither test covers every allocator or object-update crash point.
- Full task reclamation currently requires the machine that created the cap
  group (the recycler owner) to remain alive. If the owner fails, surviving
  threads fail their generation check and stop, and queued arrivals are
  discarded, but adopting the dead owner's `proc_node` and recycler authority
  for final object reclamation is not implemented.

## Cross-machine task membership and failure

A cross-machine `cap_group` is allocated in CXL and contains the task-level
failure metadata:

```c
mid_t owner_machine_id;
u64 owner_boot_generation;
volatile u64 participant_mask;
volatile u64 participant_generation[CLUSTER_MAX_MACHINE_NUM];
volatile u32 cross_machine_failure;
mid_t failed_machine_id;
```

The creator is the recycler owner and the first participant. When the durable
scheduler first dequeues one of the task's threads on another machine, it
records that machine's current generation before running the thread. A later
arrival, or the owner's periodic task scan, compares every recorded generation
with `machine_lifecycle[mid].boot_generation`.

```text
task records participant (machine 1, generation 1)
machine 1 is killed
replacement machine 1 publishes generation 2
owner observes recorded generation 1 != current generation 2
task becomes partially failed
```

The owner keeps a volatile DRAM index of cap groups it created. This index is
not authoritative persistent state; it lets the live owner scan sleeping tasks
without following machine-local pointers from CXL. The membership itself stays
inside the shared cap group.

### Coordinated termination and reclamation

Once a mismatch is published, the owner performs a single group-wide exit:

1. Threads on surviving machines change from `TE_RUNNING` to `TE_EXITING` and
   finish through their normal scheduler exit paths.
2. A thread that was running on the failed generation cannot execute an exit
   path, so it is safely completed as `TE_EXITED`, `TS_EXIT`, with its obsolete
   kernel-stack ownership cleared.
3. A `TE_MIGRATING` thread is not executing on either side. Its CXL durable
   queue node is cancelled before the thread becomes `TE_EXITED`; this prevents
   the replacement scheduler from dequeuing a freed thread pointer.
4. The owner's kernel sends the existing recycle notification to its procmgr.
5. `sys_cap_group_recycle()` waits for surviving threads, stops IPC
   registrations/connections/notifications, revokes every copy of the process
   capability, and frees the entire target capability table. Normal object
   reference counting then returns pages and CXL allocations to their existing
   allocators.

The task is never allowed to continue on the replacement incarnation. A
durable-queue dequeue first validates membership; a generation mismatch marks
the task failed and discards that arrival until the owner has reclaimed it.

This protocol deliberately does not reclaim shared cluster infrastructure such
as the scheduler queue pool. It cancels only queue nodes owned by the failed
task and leaves the queue sentinel/allocator metadata available to survivors.

## Surviving clients and failed services

A service failure is different from a cross-machine application failure. The
client process may still be healthy, so the kernel or service transport must
not kill the whole client cap group. The persistent polling transport follows
the paper's per-request DurableQueue state machine:

```text
FREE -> INIT -> DOING -> DONE
                   \-> ABORT
```

The queue and request nodes reside in CXL. A replacement polling service maps
the existing queue instead of reinitializing it, then interprets each state:

- `INIT` was published but was not claimed, so the replacement may execute it.
- `DOING` has an ambiguous outcome because the old service may have partially
  executed it. Recovery publishes `ABORT` with release ordering.
- `DONE` contains a valid response and remains available to the client.

A client waiting on `ABORT` receives `-ECONNABORTED`. It marks the old node
consumed, reconnects to the replacement service, and explicitly chooses
whether to issue a new request. The transport never automatically replays an
ambiguous mutating operation. Filesystem logging or another service-specific
protocol remains responsible for deciding whether an interrupted `DOING`
operation should be undone, redone, or reported as failed.

As with task failure, the current implementation publishes this abort when the
replacement service starts after the machine rejoins. There is no heartbeat
that can publish it during the interval between QEMU death and rejoin.

## Filesystem instance replacement and existing applications

Each persistent filesystem shard has one CXL-resident registry record:
`shard_id -> {server_thread, host_machine_id, host_boot_generation,
instance_generation, state}`. Registering a replacement service publishes a new
`instance_generation` atomically. Clients compare this value before using a
filesystem IPC connection, so the connection is rebuilt against the new service
instance instead of retaining a stale server capability.

File descriptors additionally retain the server pathname, original open flags,
current offset, shard ID, and the generation that opened them. On a generation
change, the libc file layer reconnects, reopens the pathname without
`O_CREAT/O_EXCL/O_TRUNC`, restores the offset, and then retries the requested
operation. This preserves the POSIX fd number visible to applications such as
LevelDB. It does not replay an in-flight write whose outcome was ambiguous;
LevelDB must receive an I/O error and reopen its database using its WAL recovery
policy.

## Regression test

The boot-time rejoin regression is based on the filesystem recovery artifact:

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/7-recover-fs/run-rejoin.sh
```

The test:

1. boots machines 0 and 1;
2. verifies that machine 1 is responsive;
3. kills machine 0's QEMU process;
4. starts another QEMU with `machine_id=0` without resetting CXL;
5. requires machine 0 to report `REJOINING`, generation 2, and then `ONLINE`;
6. waits for the replacement shell; and
7. verifies that machine 1 remained responsive.

The relevant log messages are:

```text
[DSM] machine 0 rejoining with boot generation 2; preserving cluster CXL state
[DSM] machine 0 online at boot generation 2 (rejoined)
```

The cross-machine task regression uses the AE 5 placement convention (one
process with workers bound across the global CPU namespace):

```bash
./artifact-evaluation/7-recover-fs/run-cross-task-rejoin.sh
```

It starts Phoenix matrix with the shell suffix `# &`, waits for a generation-1
participant on machine 1, kills that QEMU, and rejoins logical machine 1 at
generation 2. Success requires both of these owner-side markers:

```text
[CROSS_TASK] task ... lost machine 1 (participant generation 1, current generation 2)
[CROSS_TASK] recycled task ... and all capabilities
```

The service-abort regression keeps its client on machine 0 while the polling
service on machine 1 owns a request in `DOING`:

```bash
./artifact-evaluation/7-recover-fs/run-ipc-abort-rejoin.sh
```

It kills and rejoins machine 1, requires the replacement service to publish
`ABORT`, verifies that the client receives `-ECONNABORTED`, and finally maps
the replacement service queue again and completes a new request.

## Implementation map

| Concern | Source |
| --- | --- |
| Lifecycle structure and accessors | `kernel/include/dsm/dsm-single.h` |
| Join/rejoin classification and generation publication | `kernel/dsm/dsm_metadata.c` |
| CXL allocator attach path | `kernel/mm/mm.c` |
| Durable scheduler queue initialization | `kernel/sched/policy_rr.c` |
| Replacement peer-ID publication | `kernel/drivers/pci/ivshmem.c` |
| Final `ONLINE` publication | `kernel/arch/x86_64/main.c` |
| Cross-task membership and owner scan | `kernel/object/cap_group.c` |
| Group exit and failed-generation thread cleanup | `kernel/object/recycle.c` |
| Durable-queue arrival validation | `kernel/sched/policy_rr.c` |
| Service request abort/recovery | `user/system-servers/polling/polling_server.c` |
| Client abort propagation | `user/system-servers/polling/polling_req.c` |
| QEMU regression | `artifact-evaluation/7-recover-fs/run-rejoin.sh` |
| Cross-task QEMU regression | `artifact-evaluation/7-recover-fs/run-cross-task-rejoin.sh` |
| Service-abort QEMU regression | `artifact-evaluation/7-recover-fs/run-ipc-abort-rejoin.sh` |
| Global FS instance registry | `kernel/include/dsm/dsm-single.h`, `kernel/ipc/connection.c` |
| FS client generation/reopen | `user/musl-1.1.24/src/chcore-port/ipc.c`, `user/musl-1.1.24/src/chcore-port/file.c` |
