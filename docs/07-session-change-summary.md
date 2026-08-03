# Failure Recovery and Service Reconnection Design

This document describes the design for machine rejoin, cross-machine task
failure, service-request aborts, filesystem instance replacement, and client
reconnection.

## Machine identity and generations

Each logical machine has a stable machine ID and a monotonically increasing
`boot_generation`. A rejoining machine reuses its machine ID but receives a new
generation. CXL-resident metadata uses `(machine_id, boot_generation)` to
distinguish the current incarnation from stale pointers, tasks, capabilities,
and requests published by an earlier incarnation.

Volatile DRAM structures are rebuilt during boot. CXL structures containing
cluster coordination state, persistent queue nodes, filesystem metadata, and
p-log mappings are reused during rejoin.

## Cross-machine task failure

A cross-machine task records each participant's machine ID and boot generation.
When a surviving owner observes a participant generation change, it marks that
participant as failed, terminates the failed participant's execution state, and
reclaims distributed capabilities and other resources. The owner task remains
usable; the failed participant is removed from the active membership.

The recovery path must distinguish a failed participant from a whole-task
failure. Cleanup is therefore scoped to the failed participant's resources and
must be safe to repeat.

## Durable service requests

Persistent service requests use the following state machine:

```text
FREE -> INIT -> DOING -> DONE
                   \\-> ABORT
```

`INIT` means that a request was published but not claimed and may be executed by
a replacement service. `DOING` has an ambiguous outcome because the old service
may have partially executed it; recovery changes it to `ABORT`. `DONE` contains
a valid response and remains available to the client.

The client receives `-ECONNABORTED` for `ABORT`. The transport does not
automatically replay an ambiguous mutating request. The application or
service-specific recovery protocol decides whether to retry, undo, or report
the operation as failed.

## Filesystem instance registry

Each persistent filesystem shard has one CXL-resident routing record:

```text
shard_id -> {
    server_thread,
    instance_generation,
    host_machine_id,
    host_boot_generation,
    state
}
```

The `shard_id` identifies persistent filesystem data. `host_machine_id` and
`host_boot_generation` identify the current service host. Every replacement FS
service increments `instance_generation` and atomically publishes its new
server endpoint.

Clients compare the registry generation before using an FS IPC connection. A
generation mismatch invalidates the old connection and causes a new connection
to be registered against the replacement server.

## File descriptor reconnection

An application-visible file descriptor stores enough metadata to reconstruct its
server-side session:

- persistent shard ID;
- server pathname;
- original open flags and mode;
- current file offset;
- FS instance generation used by the open.

After a generation change, the client reconnects, reopens the pathname without
`O_CREAT`, `O_EXCL`, or `O_TRUNC`, restores the offset, and keeps the original
application-visible fd number. This supports applications such as LevelDB that
retain open files while the filesystem service is replaced.

An operation already in flight when the old service fails is not replayed. Its
result is ambiguous and must be surfaced as an I/O error or resolved by the
application's WAL/recovery protocol. Reconnection applies to subsequent
operations.

## LevelDB recovery model

LevelDB treats failed POSIX file operations as I/O errors. A safe recovery
sequence is:

1. report the interrupted operation as failed;
2. discard or close the affected LevelDB instance and stale file sessions;
3. wait for the replacement FS instance to publish its new generation;
4. reopen the database; and
5. use the WAL and manifest to recover committed state.

Transparent replay of an interrupted WAL write is intentionally not part of the
filesystem transport because the original write may have reached persistent
storage before the service failed.

## Failure-detection and immediate abort

The intended failure sequence is:

```text
failure detector -> mark machine/service OFFLINE
                 -> fence the failed generation's CXL/cache agent
                 -> abort affected requests and IPC calls
                 -> return an error to the application
                 -> replacement service rejoins and publishes a new generation
                 -> application reconnects and decides whether to retry
```

The abort must be published when the surviving machine detects failure, not only
when the replacement service eventually starts. A heartbeat or equivalent
failure detector should transition the machine/service registry to `OFFLINE`
and identify all connections, queue requests, and cross-machine tasks owned by
the failed generation. The kernel must wake clients blocked in remote IPC and
return an explicit aborted error. The replacement service later handles durable
queue recovery and generation publication; it must not be the first point at
which the client learns about the failure.

The current implementation has the generation and rejoin records, but its
service-request `ABORT` publication still occurs at replacement-service start.
Moving this publication into the failure-detection path is required for
immediate application-visible failure handling.

### Cache coherence, failure fencing, and the persistence boundary

CXL cache coherence, atomic visibility, and post-failure durability are three
different guarantees. If a surviving machine B successfully completes a CAS
and remains online, every other online coherent participant must be able to
observe that result; coherence provides this visibility and it does not depend
on `clwb`. If the result exists only in B's dirty cache, however, it may still
be lost if B subsequently fails. The CAS result becomes durable across B's own
failure only after `clwb` followed by `sfence` has placed it in the platform's
single-host-failure persistence domain.

Coherence cannot reconstruct an unpersisted update from failed machine A. For
example, if CXL media still contains `INIT` while A's dirty cache contains
`DOING`, A's failure may expose `INIT` again. A server must therefore persist
`DOING` before producing any request side effect:

```text
CAS(INIT -> DOING)
clwb(&status)
sfence
handle(request)
```

Recovery must not race the old cache agent. Before scanning a queue, the
failure detector must fence the failed `(machine_id, boot_generation)`, prevent
that incarnation from accessing CXL again, and ensure that its outstanding
transactions cannot arrive after recovery writes. A survivor may then CAS
`DOING` to `ABORT` and persist the result with `clwb + sfence`. If the platform
cannot clear the failed cache agent's directory ownership, or if accesses
return poison/errors, software must quarantine the queue until fabric/platform
recovery completes rather than assume that queue recovery succeeded.

The failure boundary assumed here is therefore that a failed host may lose all
CPU-cache contents while CXL memory, the fabric, and the coherence home agent
remain operational and support fencing that host. `clwb + sfence` protects a
write against a later failure of its current writer; it cannot recover dirty
cache lines already lost with another machine.

## QEMU host failure detector

`dsm-scripts/host-failure-detector.sh` is the host-side liveness
detector. It periodically probes the QEMU process with `kill -0`. On exit it
appends a durable `machine_failure` record and a compatibility
`machine<ID>_qemu_exited` marker to the event log. An optional `--notify-pid`
causes a host controller to receive `SIGUSR1`; the controller is responsible
for marking the machine offline and publishing request/task aborts. This
separates QEMU process detection from guest IPC delivery and works for every
machine ID, including a replacement instance during rejoin. With
`--restart-cmd`, the same controller starts a replacement QEMU and resumes
monitoring its PID. Before intentionally stopping the whole OS, create the
configured `--stop-file` (or terminate the detector) first; this prevents an
intentional shutdown from being mistaken for a failure and restarted.
