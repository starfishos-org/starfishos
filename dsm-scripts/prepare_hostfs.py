#!/usr/bin/env python3
"""Initialize or validate the live HostFS shared-memory protocol."""

import argparse
import mmap
import os
import struct
import sys

import hostfs_server


def protocol_is_current(device: str) -> bool:
    try:
        with open(device, "rb") as device_file:
            if os.fstat(device_file.fileno()).st_size < hostfs_server.PROTOCOL_SIZE:
                return False
            with mmap.mmap(
                device_file.fileno(), hostfs_server.HEADER_SIZE, access=mmap.ACCESS_READ
            ) as shared:
                magic, version, slots, stride, data_size = struct.unpack_from(
                    "<8sIIII", shared, 0
                )
                return (
                    magic == hostfs_server.MAGIC
                    and version == hostfs_server.VERSION
                    and slots == hostfs_server.SLOT_COUNT
                    and stride == hostfs_server.SLOT_STRIDE
                    and data_size == hostfs_server.DATA_SIZE
                )
    except OSError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--device", default=hostfs_server.default_device())
    parser.add_argument("--size", default="16G")
    args = parser.parse_args()

    if args.check:
        if protocol_is_current(args.device):
            print(f"HostFS live protocol is current: {args.device}")
            return 0
        print(f"HostFS live protocol is missing or stale: {args.device}")
        return 1

    hostfs_server.initialize_device(args.device, hostfs_server.parse_size(args.size))
    print(f"Initialized HostFS live protocol: {args.device}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
