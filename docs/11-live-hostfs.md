# Live HostFS

Live HostFS exposes a host directory as `/host` inside StarfishOS. It replaces
the original boot-time, read-only file snapshot with request forwarding: the
host directory is the only stored copy, and each guest operation acts on it
directly.

## Usage

The default host root is `datasets/`, preserving paths such as
`/host/twitter-2010.bin`. QEMU launch scripts start the service automatically.
To expose another directory:

```bash
HOSTFS_ROOT=/absolute/host/directory make run
```

The service remains alive across QEMU runs so multi-machine launches share one
host handle table and one protocol endpoint. Manage it explicitly with:

```bash
make hostfs-start
make hostfs-status
make hostfs-stop
```

Stop the service before changing `HOSTFS_ROOT`. `make clean` stops it before
removing the backing file.

## Persistent outputs and large files

Use `/host` for any guest-created artifact that must survive a StarfishOS or
QEMU restart. A recommended layout beneath the configured host root is:

```text
/host/results/<workload>/   benchmark results and checkpoints
/host/log/<workload>/       application logs written by the guest
/host/models/               model weights and other large read-mostly files
```

With the default configuration these paths are `datasets/results/`,
`datasets/log/`, and `datasets/models/` on the host. Set `HOSTFS_ROOT` when a
different host-side layout is required. The files themselves are not stored in
the 16 GiB ivshmem BAR: only request slots and 4 MiB transfer buffers are
there, so individual files may be larger than the BAR and are limited by the
host filesystem and StarfishOS `off_t` range.

For a completion marker to mean that output is durable, flush the output file
with `fsync` before publishing the marker. For replaceable checkpoints, write
a temporary file, `fsync` it, rename it over the final path, and `fsync` the
parent directory. `O_APPEND` uses the host filesystem's atomic append
operation. `fallocate` mode 0 is available for preallocating large output
files, and `statfs` reports the real capacity of the host filesystem.

## Architecture

The first part of `/dev/shm/ivshmem-hostfs-$USER` contains a versioned header,
16 concurrent request slots, and one 4 MiB transfer buffer per slot. Guest
processes map this region through `PCI_CONTROL_IVSHMEM_CONNECT`, atomically
claim a free slot, publish a request, and wait for the host daemon to complete
it. The remaining backing-file capacity is unused by HostFS v2.

The daemon maintains host file descriptors for open guest files. It supports
open/create, close, positional and sequential I/O, descriptor duplication,
`fcntl(F_GETFL/F_SETFL)`, stat/statfs, truncate, mode-0 fallocate, fsync,
directory enumeration, mkdir, unlink/rmdir, rename, access, readlink, and
symlink. Large reads and writes are split across transfer-buffer requests.
Multiple StarfishOS machines can use the same service concurrently.

The protocol header contains a server epoch and heartbeat. Guest requests wait
while the daemon is making progress, including during a long `fsync`, but fail
instead of relying on a fixed operation timeout after the daemon disappears.

All guest paths must start with `/host`. The daemon resolves them beneath the
configured root and rejects `..` or symlink traversal that would escape it. The
daemon runs with the invoking host user's permissions; choose a root that is
safe for that user to modify.

Live HostFS is intended for trusted StarfishOS workloads. Its shared request
slots are visible to guest processes that can issue the PCI control call, and
the host daemon is not a security boundary against a guest deliberately racing
path resolution. Do not expose a sensitive host directory to an untrusted
guest.

## Coherency semantics

System-call I/O has normal host-filesystem coherency. A host edit is visible to
the next guest `read` or `stat`, and a completed guest `write` has already
updated the host file. Concurrent edits follow the host filesystem's usual
rules; HostFS does not add distributed transactions or conflict resolution.

`MAP_PRIVATE` is implemented as an eager point-in-time copy because StarfishOS
cannot map arbitrary host page-cache pages through the ivshmem protocol. It
therefore consumes guest memory equal to the mapping length. Later host changes
do not update an existing guest mapping, and writes to that mapping are not
propagated. Shared file mappings return `ENOTSUP`; use `read`, `write`, or
`pwrite` when live visibility is required. For very large weights, streaming
reads avoid the extra eager mapping copy.

If the host daemon is restarted, its open-handle table is lost. Existing guest
descriptors then return `EBADF` or `ESTALE` and should be reopened. Stored host
files are unaffected.

Operations outside the supported list, including hard links, ownership/mode
changes, and shared mappings, are not yet implemented. Applications should not
silently depend on them for persistence correctness. HostFS descriptors are
also not re-registered with the daemon across `fork`/`exec`; multi-process
programs should open their `/host` files independently in each process.
