# DurableQueue crash-recovery unit test

This is a host-side unit test for the cross-machine IPC DurableQueue. It does
not boot StarfishOS or QEMU.

The test keeps two queue images:

- `live`: CPU-visible state, including dirty cache-line updates;
- `durable`: CXL state that has passed a modeled `clwb + sfence`.

A simulated machine crash discards `live` and restores it from `durable`.
This makes missing persistence operations observable on an ordinary host,
where killing a process alone would not discard the host CPU caches.

Covered cases:

- a published `INIT` request remains executable after a crash;
- `DOING` becomes `ABORT` and is visible to the client;
- a persisted response without persisted `DONE` is conservatively aborted;
- `DONE` and its response survive together;
- `head` is durable before its old sentinel is recycled;
- `tail` is durable before a durable `head` can pass it;
- allocator pop/push publication remains consistent across crashes.

Run with:

```sh
./artifact-evaluation/10-recover-unit-tests/run.sh
```
