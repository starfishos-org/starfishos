#!/usr/bin/env python3
"""Serve StarfishOS /host requests from a real host directory."""

import argparse
import errno
import fcntl
import json
import mmap
import os
import signal
import stat
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path


MAGIC = b"hostfs2\0"
VERSION = 2
HEADER_SIZE = 4096
SLOT_HEADER_SIZE = 4096
PATH_SIZE = 512
SLOT_COUNT = 16
DATA_SIZE = 4 * 1024 * 1024
SLOT_STRIDE = SLOT_HEADER_SIZE + DATA_SIZE
PROTOCOL_SIZE = HEADER_SIZE + SLOT_COUNT * SLOT_STRIDE
DEFAULT_DEVICE_SIZE = 16 * 1024 * 1024 * 1024

STATE_FREE = 0
STATE_CLAIMED = 1
STATE_READY = 2
STATE_DONE = 3

OP_OPEN = 1
OP_CLOSE = 2
OP_PREAD = 3
OP_PWRITE = 4
OP_STAT = 5
OP_FSTAT = 6
OP_FSYNC = 7
OP_FTRUNCATE = 8
OP_GETDENTS = 9
OP_MKDIR = 10
OP_UNLINK = 11
OP_RMDIR = 12
OP_RENAME = 13
OP_ACCESS = 14
OP_READLINK = 15
OP_SYMLINK = 16
OP_FCNTL = 17
OP_FALLOCATE = 18
OP_STATFS = 19

IO_APPEND = 1 << 0
HEARTBEAT_INTERVAL = 0.25

OFF_STATE = 0
OFF_SEQUENCE = 4
OFF_OPCODE = 8
OFF_FLAGS = 12
OFF_RESULT = 16
OFF_HANDLE = 24
OFF_OFFSET = 32
OFF_COUNT = 40
OFF_SIZE = 48
OFF_MODE = 56
OFF_INO = 64
OFF_ATIME_NS = 72
OFF_MTIME_NS = 80
OFF_CTIME_NS = 88
OFF_PATH_LENGTH = 96
OFF_UID = 100
OFF_GID = 104
OFF_NLINK = 108
OFF_BLOCKS = 112
OFF_BLOCK_SIZE = 120
OFF_PATH = 128
HEADER_OFF_HEARTBEAT = 32

DIRENT_SIZE = 280
AT_SYMLINK_NOFOLLOW = 0x100


def default_device() -> str:
    return f"/dev/shm/ivshmem-hostfs-{os.environ.get('USER', os.getuid())}"


def default_root() -> str:
    repo_root = Path(__file__).resolve().parent.parent
    return str(repo_root / "datasets")


def runtime_paths(device: str) -> tuple[str, str, str, str]:
    tag = Path(device).name.replace("/", "_")
    base = f"/tmp/starfish-{tag}"
    return base + ".pid", base + ".status", base + ".lock", base + ".log"


def process_alive(pid: int) -> bool:
    if pid <= 1:
        return False
    try:
        os.kill(pid, 0)
    except (OSError, ValueError):
        return False
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii") as stat_file:
            if stat_file.read().split()[2] == "Z":
                return False
    except (OSError, IndexError):
        pass
    return True


def server_process_alive(pid: int, device: str) -> bool:
    if not process_alive(pid):
        return False
    try:
        command = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    except OSError:
        return False
    script = os.fsencode(os.path.realpath(__file__))
    device_arg = os.fsencode(os.path.realpath(device))
    return script in command and b"serve" in command and device_arg in command


def read_status(device: str) -> dict:
    _, status_path, _, _ = runtime_paths(device)
    try:
        with open(status_path, encoding="utf-8") as status_file:
            return json.load(status_file)
    except (OSError, ValueError):
        return {}


def write_status(device: str, root: str) -> None:
    pid_path, status_path, _, _ = runtime_paths(device)
    status = {"pid": os.getpid(), "device": device, "root": root, "version": VERSION}
    tmp_path = status_path + f".{os.getpid()}"
    with open(tmp_path, "w", encoding="utf-8") as status_file:
        json.dump(status, status_file)
        status_file.write("\n")
    os.replace(tmp_path, status_path)
    with open(pid_path, "w", encoding="ascii") as pid_file:
        pid_file.write(f"{os.getpid()}\n")


def parse_size(value: str) -> int:
    suffixes = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}
    value = value.strip().lower()
    if value[-1:] in suffixes:
        return int(value[:-1]) * suffixes[value[-1]]
    return int(value)


def initialize_device(device: str, size: int = DEFAULT_DEVICE_SIZE) -> None:
    os.makedirs(os.path.dirname(device), exist_ok=True)
    fd = os.open(device, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        current_size = os.fstat(fd).st_size
        required_size = max(size, PROTOCOL_SIZE)
        if current_size < required_size:
            os.ftruncate(fd, required_size)
        with mmap.mmap(fd, PROTOCOL_SIZE, access=mmap.ACCESS_WRITE) as shared:
            empty_header = b"\0" * HEADER_SIZE
            shared[:HEADER_SIZE] = empty_header
            for index in range(SLOT_COUNT):
                slot = HEADER_SIZE + index * SLOT_STRIDE
                shared[slot : slot + SLOT_HEADER_SIZE] = empty_header
            struct.pack_into(
                "<8sIIIIQQ",
                shared,
                0,
                MAGIC,
                VERSION,
                SLOT_COUNT,
                SLOT_STRIDE,
                DATA_SIZE,
                time.time_ns(),
                1,
            )
            shared.flush()
    finally:
        os.close(fd)


class HostFSServer:
    def __init__(self, device: str, root: str, poll_interval: float):
        self.device = os.path.realpath(device)
        self.root = os.path.realpath(root)
        self.poll_interval = poll_interval
        self.running = True
        self.handles: dict[int, int] = {}
        # Make stale guest handles from an earlier daemon instance extremely
        # unlikely to alias a newly opened file after a server restart.
        self.next_handle = time.time_ns() & ((1 << 63) - 1) or 1
        self.shared = None
        self.device_fd = -1
        self.heartbeat_thread = None

    def stop(self, _signum=None, _frame=None) -> None:
        self.running = False

    def update_heartbeat(self) -> None:
        heartbeat = 1
        while self.running:
            heartbeat += 1
            struct.pack_into("<Q", self.shared, HEADER_OFF_HEARTBEAT, heartbeat)
            time.sleep(HEARTBEAT_INTERVAL)

    def resolve(
        self, guest_path: str, allow_missing: bool = False, follow_final: bool = True
    ) -> str:
        if guest_path == "/host":
            relative = "."
        elif guest_path.startswith("/host/"):
            relative = guest_path[6:]
        else:
            raise OSError(errno.EINVAL, "path is outside /host")
        if "\0" in relative:
            raise OSError(errno.EINVAL, "path contains NUL")

        candidate = os.path.join(self.root, relative)
        if (allow_missing and not os.path.lexists(candidate)) or not follow_final:
            resolved_parent = os.path.realpath(os.path.dirname(candidate))
            resolved = os.path.join(resolved_parent, os.path.basename(candidate))
        else:
            resolved = os.path.realpath(candidate)
        try:
            inside = os.path.commonpath((self.root, resolved)) == self.root
        except ValueError:
            inside = False
        if not inside:
            raise OSError(errno.EACCES, "path escapes the HostFS root")
        return resolved

    def allocate_handle(self, fd: int) -> int:
        handle = self.next_handle
        self.next_handle += 1
        self.handles[handle] = fd
        return handle

    def get_fd(self, handle: int) -> int:
        try:
            return self.handles[handle]
        except KeyError as exc:
            raise OSError(errno.EBADF, "unknown HostFS handle") from exc

    def slot_base(self, index: int) -> int:
        return HEADER_SIZE + index * SLOT_STRIDE

    def unpack_u32(self, base: int, field: int) -> int:
        return struct.unpack_from("<I", self.shared, base + field)[0]

    def unpack_u64(self, base: int, field: int) -> int:
        return struct.unpack_from("<Q", self.shared, base + field)[0]

    def pack_u32(self, base: int, field: int, value: int) -> None:
        struct.pack_into("<I", self.shared, base + field, value)

    def pack_u64(self, base: int, field: int, value: int) -> None:
        struct.pack_into("<Q", self.shared, base + field, value)

    def path_from_slot(self, base: int) -> str:
        length = self.unpack_u32(base, OFF_PATH_LENGTH)
        if length >= PATH_SIZE:
            raise OSError(errno.ENAMETOOLONG, "HostFS path is too long")
        raw = self.shared[base + OFF_PATH : base + OFF_PATH + length]
        return raw.decode("utf-8", "surrogateescape")

    def data_range(self, base: int, length: int) -> tuple[int, int]:
        if length > DATA_SIZE:
            raise OSError(errno.E2BIG, "HostFS request exceeds slot data size")
        start = base + SLOT_HEADER_SIZE
        return start, start + length

    def set_stat(self, base: int, st: os.stat_result) -> None:
        self.pack_u64(base, OFF_SIZE, st.st_size)
        self.pack_u64(base, OFF_MODE, st.st_mode)
        self.pack_u64(base, OFF_INO, st.st_ino)
        self.pack_u64(base, OFF_ATIME_NS, st.st_atime_ns)
        self.pack_u64(base, OFF_MTIME_NS, st.st_mtime_ns)
        self.pack_u64(base, OFF_CTIME_NS, st.st_ctime_ns)
        self.pack_u32(base, OFF_UID, st.st_uid)
        self.pack_u32(base, OFF_GID, st.st_gid)
        self.pack_u32(base, OFF_NLINK, st.st_nlink)
        self.pack_u64(base, OFF_BLOCKS, st.st_blocks)
        self.pack_u64(base, OFF_BLOCK_SIZE, st.st_blksize)

    @staticmethod
    def dirent_type(mode: int) -> int:
        return {
            stat.S_IFIFO: 1,
            stat.S_IFCHR: 2,
            stat.S_IFDIR: 4,
            stat.S_IFBLK: 6,
            stat.S_IFREG: 8,
            stat.S_IFLNK: 10,
            stat.S_IFSOCK: 12,
        }.get(stat.S_IFMT(mode), 0)

    def encode_dirents(
        self, fd: int, start_index: int, capacity: int
    ) -> tuple[bytes, int]:
        directory_stat = os.fstat(fd)
        entries = [
            (".", directory_stat.st_ino, stat.S_IFDIR),
            ("..", directory_stat.st_ino, stat.S_IFDIR),
        ]
        with os.scandir(fd) as iterator:
            for entry in sorted(iterator, key=lambda item: item.name):
                entry_stat = entry.stat(follow_symlinks=False)
                entries.append((entry.name, entry_stat.st_ino, entry_stat.st_mode))

        output = bytearray()
        index = start_index
        while index < len(entries) and len(output) + DIRENT_SIZE <= capacity:
            name, inode, mode = entries[index]
            encoded_name = name.encode("utf-8", "surrogateescape")[:255]
            record = bytearray(DIRENT_SIZE)
            struct.pack_into(
                "<QqHB",
                record,
                0,
                inode,
                index + 1,
                DIRENT_SIZE,
                self.dirent_type(mode),
            )
            record[19 : 19 + len(encoded_name)] = encoded_name
            output.extend(record)
            index += 1
        return bytes(output), index

    def process(self, index: int) -> None:
        base = self.slot_base(index)
        opcode = self.unpack_u32(base, OFF_OPCODE)
        flags = self.unpack_u32(base, OFF_FLAGS)
        handle = self.unpack_u64(base, OFF_HANDLE)
        offset = self.unpack_u64(base, OFF_OFFSET)
        count = self.unpack_u64(base, OFF_COUNT)
        mode = self.unpack_u64(base, OFF_MODE)
        result = 0

        try:
            if opcode == OP_OPEN:
                path = self.resolve(self.path_from_slot(base), bool(flags & os.O_CREAT))
                fd = os.open(path, flags, mode & 0o7777)
                handle = self.allocate_handle(fd)
                self.pack_u64(base, OFF_HANDLE, handle)
                self.set_stat(base, os.fstat(fd))
            elif opcode == OP_CLOSE:
                fd = self.get_fd(handle)
                os.close(fd)
                del self.handles[handle]
            elif opcode == OP_PREAD:
                fd = self.get_fd(handle)
                data = os.pread(fd, min(count, DATA_SIZE), offset)
                start, end = self.data_range(base, len(data))
                self.shared[start:end] = data
                result = len(data)
                self.set_stat(base, os.fstat(fd))
            elif opcode == OP_PWRITE:
                fd = self.get_fd(handle)
                start, end = self.data_range(base, count)
                if flags & IO_APPEND:
                    result = os.write(fd, self.shared[start:end])
                    self.pack_u64(base, OFF_OFFSET, os.lseek(fd, 0, os.SEEK_CUR))
                else:
                    status_flags = fcntl.fcntl(fd, fcntl.F_GETFL)
                    restore_append = bool(status_flags & os.O_APPEND)
                    if restore_append:
                        fcntl.fcntl(fd, fcntl.F_SETFL, status_flags & ~os.O_APPEND)
                    try:
                        result = os.pwrite(fd, self.shared[start:end], offset)
                    finally:
                        if restore_append:
                            fcntl.fcntl(fd, fcntl.F_SETFL, status_flags)
                self.set_stat(base, os.fstat(fd))
            elif opcode == OP_STAT:
                nofollow = bool(flags & AT_SYMLINK_NOFOLLOW)
                path = self.resolve(
                    self.path_from_slot(base), follow_final=not nofollow
                )
                self.set_stat(
                    base,
                    os.stat(
                        path, follow_symlinks=not bool(flags & AT_SYMLINK_NOFOLLOW)
                    ),
                )
            elif opcode == OP_FSTAT:
                self.set_stat(base, os.fstat(self.get_fd(handle)))
            elif opcode == OP_FSYNC:
                os.fsync(self.get_fd(handle))
            elif opcode == OP_FTRUNCATE:
                fd = self.get_fd(handle)
                os.ftruncate(fd, offset)
                self.set_stat(base, os.fstat(fd))
            elif opcode == OP_GETDENTS:
                data, next_index = self.encode_dirents(
                    self.get_fd(handle), offset, min(count, DATA_SIZE)
                )
                start, end = self.data_range(base, len(data))
                self.shared[start:end] = data
                self.pack_u64(base, OFF_OFFSET, next_index)
                result = len(data)
            elif opcode == OP_MKDIR:
                os.mkdir(self.resolve(self.path_from_slot(base), True), mode & 0o7777)
            elif opcode == OP_UNLINK:
                os.unlink(self.resolve(self.path_from_slot(base), follow_final=False))
            elif opcode == OP_RMDIR:
                os.rmdir(self.resolve(self.path_from_slot(base), follow_final=False))
            elif opcode == OP_RENAME:
                old_path = self.resolve(self.path_from_slot(base), follow_final=False)
                start, end = self.data_range(base, count)
                new_guest_path = self.shared[start:end].decode(
                    "utf-8", "surrogateescape"
                )
                os.rename(old_path, self.resolve(new_guest_path, True))
            elif opcode == OP_ACCESS:
                path = self.resolve(self.path_from_slot(base))
                follow_symlinks = not bool(flags & AT_SYMLINK_NOFOLLOW)
                os.stat(path, follow_symlinks=follow_symlinks)
                if not os.access(path, mode, follow_symlinks=follow_symlinks):
                    raise OSError(errno.EACCES, "access denied")
            elif opcode == OP_READLINK:
                target = os.readlink(
                    self.resolve(self.path_from_slot(base), follow_final=False)
                )
                data = os.fsencode(target)[: min(count, DATA_SIZE)]
                start, end = self.data_range(base, len(data))
                self.shared[start:end] = data
                result = len(data)
            elif opcode == OP_SYMLINK:
                start, end = self.data_range(base, count)
                target = self.shared[start:end].decode("utf-8", "surrogateescape")
                os.symlink(target, self.resolve(self.path_from_slot(base), True))
            elif opcode == OP_FCNTL:
                result = fcntl.fcntl(self.get_fd(handle), mode, flags)
            elif opcode == OP_FALLOCATE:
                if flags != 0:
                    raise OSError(errno.EOPNOTSUPP, "unsupported fallocate mode")
                fd = self.get_fd(handle)
                os.posix_fallocate(fd, offset, mode)
                self.set_stat(base, os.fstat(fd))
            elif opcode == OP_STATFS:
                filesystem = os.statvfs(self.root)
                self.pack_u64(base, OFF_SIZE, filesystem.f_blocks)
                self.pack_u64(base, OFF_MODE, filesystem.f_bfree)
                self.pack_u64(base, OFF_INO, filesystem.f_bavail)
                self.pack_u64(base, OFF_ATIME_NS, filesystem.f_files)
                self.pack_u64(base, OFF_MTIME_NS, filesystem.f_ffree)
                self.pack_u64(base, OFF_BLOCKS, filesystem.f_frsize)
                self.pack_u64(base, OFF_BLOCK_SIZE, filesystem.f_bsize)
                self.pack_u32(base, OFF_NLINK, filesystem.f_namemax)
            else:
                raise OSError(errno.ENOSYS, f"unknown HostFS opcode {opcode}")
        except OSError as error:
            result = -(error.errno or errno.EIO)
        except (UnicodeError, ValueError, OverflowError):
            result = -errno.EINVAL

        struct.pack_into("<q", self.shared, base + OFF_RESULT, result)
        self.pack_u32(base, OFF_STATE, STATE_DONE)

    def run(self) -> None:
        os.makedirs(self.root, exist_ok=True)
        initialize_device(self.device)
        self.device_fd = os.open(self.device, os.O_RDWR)
        self.shared = mmap.mmap(self.device_fd, PROTOCOL_SIZE, access=mmap.ACCESS_WRITE)
        signal.signal(signal.SIGTERM, self.stop)
        signal.signal(signal.SIGINT, self.stop)
        self.heartbeat_thread = threading.Thread(
            target=self.update_heartbeat, name="hostfs-heartbeat", daemon=True
        )
        self.heartbeat_thread.start()
        write_status(self.device, self.root)
        print(
            f"HostFS v{VERSION} serving {self.root} through {self.device}", flush=True
        )

        try:
            while self.running:
                handled = False
                for index in range(SLOT_COUNT):
                    base = self.slot_base(index)
                    if self.unpack_u32(base, OFF_STATE) == STATE_READY:
                        self.process(index)
                        handled = True
                if not handled:
                    time.sleep(self.poll_interval)
        finally:
            self.running = False
            if self.heartbeat_thread is not None:
                self.heartbeat_thread.join(timeout=2 * HEARTBEAT_INTERVAL)
            for fd in self.handles.values():
                try:
                    os.close(fd)
                except OSError:
                    pass
            self.shared.close()
            os.close(self.device_fd)


def serve(device: str, root: str, poll_interval: float) -> int:
    device = os.path.realpath(device)
    root = os.path.realpath(root)
    _, _, lock_path, _ = runtime_paths(device)
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0
        HostFSServer(device, root, poll_interval).run()
        return 0
    finally:
        os.close(lock_fd)


def ensure(device: str, root: str, poll_interval: float) -> int:
    device = os.path.realpath(device)
    root = os.path.realpath(root)
    pid_path, _, lock_path, log_path = runtime_paths(device)
    ensure_lock_path = lock_path + ".ensure"
    ensure_fd = os.open(ensure_lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(ensure_fd, fcntl.LOCK_EX)
        status = read_status(device)
        pid = int(status.get("pid", 0))
        if server_process_alive(pid, device):
            if status.get("root") != root:
                print(
                    f"HostFS is already serving {status.get('root')}; requested {root}. "
                    f"Run '{Path(__file__).name} stop' before changing HOSTFS_ROOT.",
                    file=sys.stderr,
                )
                return 1
            return 0

        os.makedirs(root, exist_ok=True)
        log_file = open(log_path, "ab", buffering=0)
        command = [
            sys.executable,
            os.path.realpath(__file__),
            "serve",
            "--device",
            device,
            "--root",
            root,
            "--poll-interval",
            str(poll_interval),
        ]
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            close_fds=True,
        )
        log_file.close()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            status = read_status(device)
            if int(status.get("pid", 0)) == process.pid and server_process_alive(
                process.pid, device
            ):
                return 0
            if process.poll() is not None:
                break
            time.sleep(0.02)
        print(f"HostFS server failed to start; see {log_path}", file=sys.stderr)
        return 1
    finally:
        os.close(ensure_fd)


def stop(device: str) -> int:
    device = os.path.realpath(device)
    pid_path, status_path, _, _ = runtime_paths(device)
    status = read_status(device)
    pid = int(status.get("pid", 0))
    if server_process_alive(pid, device):
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 5.0
        while server_process_alive(pid, device) and time.monotonic() < deadline:
            time.sleep(0.05)
        if server_process_alive(pid, device):
            print(f"HostFS server {pid} did not stop", file=sys.stderr)
            return 1
    for path in (pid_path, status_path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command", choices=("serve", "ensure", "init", "stop", "status")
    )
    parser.add_argument("--device", default=default_device())
    parser.add_argument("--root", default=os.environ.get("HOSTFS_ROOT", default_root()))
    parser.add_argument("--size", default="16G")
    parser.add_argument("--poll-interval", type=float, default=0.0002)
    args = parser.parse_args()

    if args.command == "serve":
        return serve(args.device, args.root, args.poll_interval)
    if args.command == "ensure":
        return ensure(args.device, args.root, args.poll_interval)
    if args.command == "init":
        initialize_device(os.path.realpath(args.device), parse_size(args.size))
        return 0
    if args.command == "stop":
        return stop(args.device)

    status = read_status(os.path.realpath(args.device))
    pid = int(status.get("pid", 0))
    if server_process_alive(pid, os.path.realpath(args.device)):
        print(json.dumps(status, sort_keys=True))
        return 0
    print("HostFS server is not running", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
