#!/usr/bin/env python3
"""Spawn one detached daemon without leaking the caller's file descriptors."""

import argparse
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a daemon command is required after --")

    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open("wb") as output:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            close_fds=True,
            start_new_session=True,
        )
    print(process.pid, flush=True)


if __name__ == "__main__":
    main()
