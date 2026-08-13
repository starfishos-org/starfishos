#!/usr/bin/env python3
"""Protocol-level tests for the live HostFS host daemon."""

import errno
import fcntl
import mmap
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "dsm-scripts"))
import hostfs_server as protocol  # noqa: E402


class HostFSServerTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name) / "root"
        self.root.mkdir()
        self.device = str(Path(self.tempdir.name) / "hostfs.mem")
        protocol.initialize_device(self.device, protocol.PROTOCOL_SIZE)

        self.server = protocol.HostFSServer(self.device, str(self.root), 0.001)
        self.server.device_fd = os.open(self.device, os.O_RDWR)
        self.server.shared = mmap.mmap(
            self.server.device_fd, protocol.PROTOCOL_SIZE, access=mmap.ACCESS_WRITE
        )
        self.base = self.server.slot_base(0)

    def tearDown(self):
        for fd in self.server.handles.values():
            os.close(fd)
        self.server.shared.close()
        os.close(self.server.device_fd)
        self.tempdir.cleanup()

    def request(
        self,
        opcode,
        path="",
        *,
        handle=0,
        flags=0,
        offset=0,
        mode=0,
        data=b"",
        count=None,
    ):
        shared = self.server.shared
        shared[self.base : self.base + protocol.SLOT_HEADER_SIZE] = (
            b"\0" * protocol.SLOT_HEADER_SIZE
        )
        encoded_path = path.encode()
        struct.pack_into(
            "<I", shared, self.base + protocol.OFF_STATE, protocol.STATE_READY
        )
        struct.pack_into("<I", shared, self.base + protocol.OFF_OPCODE, opcode)
        struct.pack_into("<I", shared, self.base + protocol.OFF_FLAGS, flags)
        struct.pack_into("<Q", shared, self.base + protocol.OFF_HANDLE, handle)
        struct.pack_into("<Q", shared, self.base + protocol.OFF_OFFSET, offset)
        struct.pack_into(
            "<Q",
            shared,
            self.base + protocol.OFF_COUNT,
            len(data) if count is None else count,
        )
        struct.pack_into("<Q", shared, self.base + protocol.OFF_MODE, mode)
        struct.pack_into(
            "<I", shared, self.base + protocol.OFF_PATH_LENGTH, len(encoded_path)
        )
        shared[
            self.base
            + protocol.OFF_PATH : self.base
            + protocol.OFF_PATH
            + len(encoded_path)
        ] = encoded_path
        data_start = self.base + protocol.SLOT_HEADER_SIZE
        shared[data_start : data_start + len(data)] = data

        self.server.process(0)
        result = struct.unpack_from("<q", shared, self.base + protocol.OFF_RESULT)[0]
        returned_handle = struct.unpack_from(
            "<Q", shared, self.base + protocol.OFF_HANDLE
        )[0]
        returned_data = bytes(shared[data_start : data_start + max(result, 0)])
        return result, returned_handle, returned_data

    def test_guest_writes_and_host_edits_are_immediately_visible(self):
        result, handle, _ = self.request(
            protocol.OP_OPEN,
            "/host/live.txt",
            flags=os.O_CREAT | os.O_RDWR | os.O_TRUNC,
            mode=0o644,
        )
        self.assertEqual(result, 0)

        result, _, _ = self.request(
            protocol.OP_PWRITE, handle=handle, offset=0, data=b"from guest"
        )
        self.assertEqual(result, len(b"from guest"))
        self.assertEqual((self.root / "live.txt").read_bytes(), b"from guest")

        result, _, _ = self.request(
            protocol.OP_FCNTL, handle=handle, mode=fcntl.F_GETFL
        )
        self.assertEqual(result & os.O_ACCMODE, os.O_RDWR)
        self.assertEqual(
            self.request(
                protocol.OP_FCNTL,
                handle=handle,
                flags=result | os.O_APPEND,
                mode=fcntl.F_SETFL,
            )[0],
            0,
        )
        self.assertTrue(
            fcntl.fcntl(self.server.handles[handle], fcntl.F_GETFL) & os.O_APPEND
        )

        result, _, _ = self.request(
            protocol.OP_PWRITE,
            handle=handle,
            flags=protocol.IO_APPEND,
            offset=0,
            data=b"!",
        )
        self.assertEqual(result, 1)
        self.assertEqual((self.root / "live.txt").read_bytes(), b"from guest!")

        result, _, _ = self.request(
            protocol.OP_PWRITE, handle=handle, offset=0, data=b"F"
        )
        self.assertEqual(result, 1)
        self.assertEqual((self.root / "live.txt").read_bytes(), b"From guest!")

        (self.root / "live.txt").write_bytes(b"from host")
        result, _, data = self.request(
            protocol.OP_PREAD, handle=handle, offset=0, count=64
        )
        self.assertEqual(result, len(b"from host"))
        self.assertEqual(data, b"from host")

    def test_directories_and_rename(self):
        self.assertEqual(
            self.request(protocol.OP_MKDIR, "/host/work", mode=0o755)[0], 0
        )
        _, handle, _ = self.request(
            protocol.OP_OPEN, "/host/work", flags=os.O_RDONLY | os.O_DIRECTORY
        )
        result, _, data = self.request(
            protocol.OP_GETDENTS, handle=handle, offset=0, count=4096
        )
        self.assertGreaterEqual(result, 2 * protocol.DIRENT_SIZE)
        self.assertEqual(data[19:20], b".")

        (self.root / "old").write_text("data", encoding="utf-8")
        self.assertEqual(
            self.request(
                protocol.OP_RENAME,
                "/host/old",
                data=b"/host/new",
            )[0],
            0,
        )
        self.assertTrue((self.root / "new").is_file())

    def test_preallocation_and_filesystem_capacity(self):
        _, handle, _ = self.request(
            protocol.OP_OPEN,
            "/host/preallocated.bin",
            flags=os.O_CREAT | os.O_RDWR,
            mode=0o600,
        )
        self.assertEqual(
            self.request(
                protocol.OP_FALLOCATE,
                handle=handle,
                offset=4096,
                mode=8192,
            )[0],
            0,
        )
        self.assertEqual((self.root / "preallocated.bin").stat().st_size, 12288)

        self.assertEqual(self.request(protocol.OP_STATFS)[0], 0)
        blocks = struct.unpack_from(
            "<Q", self.server.shared, self.base + protocol.OFF_SIZE
        )[0]
        block_size = struct.unpack_from(
            "<Q", self.server.shared, self.base + protocol.OFF_BLOCK_SIZE
        )[0]
        self.assertGreater(blocks, 0)
        self.assertGreater(block_size, 0)

    def test_path_escape_is_rejected(self):
        outside = Path(self.tempdir.name) / "outside"
        outside.write_text("secret", encoding="utf-8")
        result, _, _ = self.request(protocol.OP_STAT, "/host/../outside")
        self.assertEqual(result, -errno.EACCES)

        os.symlink(outside, self.root / "escape")
        result, _, _ = self.request(protocol.OP_OPEN, "/host/escape", flags=os.O_RDWR)
        self.assertEqual(result, -errno.EACCES)

        result, _, _ = self.request(protocol.OP_ACCESS, "/host/missing")
        self.assertEqual(result, -errno.ENOENT)

    def test_missing_pid_is_not_treated_as_a_running_server(self):
        self.assertFalse(protocol.process_alive(0))
        self.assertFalse(protocol.process_alive(-1))
        self.assertFalse(protocol.server_process_alive(os.getpid(), self.device))


if __name__ == "__main__":
    unittest.main()
