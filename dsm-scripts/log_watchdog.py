#!/usr/bin/env python3
"""Host-side guest-log watchdog for StarfishOS test runs.

This process runs on the *host*, never inside QEMU.  It tails the per-machine
serial logs that build/simulate.sh writes and fails the run as soon as a fatal
guest signature shows up on any machine, instead of letting the test sit until
its own timeout expires.  Test launchers (artifact-evaluation/common.sh,
dsm-scripts/simulate_ncluster.sh) start one watchdog per cluster boot and poll
the flag file it writes.

On detection it:
  1. writes <flag-file> with the machine, log path and matched line,
  2. prints the offending line (plus trailing context) to stderr,
  3. optionally signals the launcher PIDs given with --kill-pid,
  4. exits 1.

Standalone use (e.g. next to a manual `make r2`):

    ./dsm-scripts/log_watchdog.py --log-dir logs --count 2 \\
        --flag-file /tmp/starfish-watchdog.flag

Exit codes: 0 = stopped without finding anything (SIGTERM/SIGINT), 1 = fatal
guest signature detected, 2 = bad usage / unusable log directory.
"""

import argparse
import os
import re
import signal
import sys
import time
from datetime import datetime

# Keep in sync with AE_ERROR_PATTERN in artifact-evaluation/common.sh.  That
# file stays the single source of truth for artifact runs (it passes --pattern
# explicitly); this default only serves standalone invocations.
DEFAULT_PATTERN = (
    r"General Protection Fault|Kernel panic|kernel panic|panic:|BUG:|BUG_ON"
    r"|Unhandled .*[Ee]xception|Unhandled .*fault|pool=NULL for va|KERNEL FAULT"
    r"|Trap No\. |Persistent data verification failed"
    r"|do_page_fault: invalid user access|do_page_fault: user NULL dereference"
)

# Context kept per machine so the report shows how the guest got there.
CONTEXT_LINES = 12

_stop = False


def _handle_stop(_signum, _frame):
    global _stop
    _stop = True


class LogTail:
    """Incremental reader for one machine log.

    Handles the file not existing yet, being recreated by the next boot
    (inode change) and being truncated, all of which happen between the
    reboots a sweep does.
    """

    def __init__(self, machine, path, start_at_end=False):
        self.machine = machine
        self.path = path
        self.offset = 0
        self.inode = None
        self.partial = ""
        self.start_at_end = start_at_end
        self.context = []

    def _reopen_if_needed(self, st):
        if self.inode is None:
            self.inode = st.st_ino
            if self.start_at_end:
                self.offset = st.st_size
            return
        if st.st_ino != self.inode or st.st_size < self.offset:
            # New boot wrote a fresh log, or the file was truncated.
            self.inode = st.st_ino
            self.offset = 0
            self.partial = ""
            self.context = []

    def read_new_lines(self):
        try:
            st = os.stat(self.path)
        except OSError:
            return []
        self._reopen_if_needed(st)
        if st.st_size <= self.offset:
            return []
        try:
            with open(self.path, "rb") as fp:
                fp.seek(self.offset)
                chunk = fp.read(st.st_size - self.offset)
                self.offset = fp.tell()
        except OSError:
            return []

        text = self.partial + chunk.decode("utf-8", errors="replace")
        lines = text.split("\n")
        self.partial = lines.pop()
        return lines


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log-dir", required=True,
                    help="directory holding exec_log<N>.log")
    ap.add_argument("--prefix", default="exec_log",
                    help="log file prefix (default: exec_log)")
    ap.add_argument("--count", type=int, default=0,
                    help="watch machines 0..COUNT-1")
    ap.add_argument("--machines", default="",
                    help="explicit comma-separated machine list "
                         "(overrides --count; use to drop a machine that the "
                         "test kills on purpose)")
    ap.add_argument("--pattern", default=DEFAULT_PATTERN,
                    help="fatal signature regex")
    ap.add_argument("--ignore", action="append", default=[],
                    help="regex for lines to ignore even if they match "
                         "--pattern (repeatable)")
    ap.add_argument("--flag-file", required=True,
                    help="written when a fatal signature is detected")
    ap.add_argument("--status-log", default="",
                    help="append watchdog activity here as well as stderr")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="poll interval in seconds (default: 1.0)")
    ap.add_argument("--kill-pid", action="append", default=[],
                    help="PID to SIGTERM on detection (repeatable)")
    ap.add_argument("--label", default="",
                    help="run label included in the report")
    ap.add_argument("--start-at-end", action="store_true",
                    help="ignore log content already present at startup")
    args = ap.parse_args(argv)

    if args.machines:
        try:
            args.machine_ids = [int(m) for m in args.machines.split(",") if m.strip() != ""]
        except ValueError:
            ap.error("--machines must be a comma-separated list of integers")
    elif args.count > 0:
        args.machine_ids = list(range(args.count))
    else:
        ap.error("one of --count or --machines is required")
    if not args.machine_ids:
        ap.error("no machines to watch")
    return args


def main(argv):
    args = parse_args(argv)
    signal.signal(signal.SIGTERM, _handle_stop)
    signal.signal(signal.SIGINT, _handle_stop)

    if not os.path.isdir(args.log_dir):
        try:
            os.makedirs(args.log_dir, exist_ok=True)
        except OSError as exc:
            print("[WATCHDOG] unusable log directory %s: %s" % (args.log_dir, exc),
                  file=sys.stderr)
            return 2

    fatal_re = re.compile(args.pattern)
    ignore_res = [re.compile(p) for p in args.ignore]

    status = None
    if args.status_log:
        try:
            status = open(args.status_log, "a", buffering=1)
        except OSError:
            status = None

    def report(msg):
        print(msg, file=sys.stderr)
        sys.stderr.flush()
        if status:
            status.write(msg + "\n")

    tails = [
        LogTail(m, os.path.join(args.log_dir, "%s%d.log" % (args.prefix, m)),
                args.start_at_end)
        for m in args.machine_ids
    ]

    label = args.label or "run"
    report("[WATCHDOG] host log watchdog started for %s: machines %s in %s "
           "(interval %gs, pid %d)"
           % (label, ",".join(str(m) for m in args.machine_ids),
              args.log_dir, args.interval, os.getpid()))

    # A leftover flag from a previous run would abort this one immediately.
    try:
        os.remove(args.flag_file)
    except OSError:
        pass

    # A launcher that is killed outright (SIGKILL, or an interrupted
    # run_all.py) never gets to stop us, so exit when we are reparented
    # instead of polling these logs forever.
    parent_pid = os.getppid()

    while not _stop:
        if os.getppid() != parent_pid:
            report("[WATCHDOG] launcher (pid %d) is gone; stopping" % parent_pid)
            if status:
                status.close()
            return 0
        for tail in tails:
            for line in tail.read_new_lines():
                tail.context.append(line)
                if len(tail.context) > CONTEXT_LINES:
                    tail.context.pop(0)
                match = fatal_re.search(line)
                if not match:
                    continue
                if any(ign.search(line) for ign in ignore_res):
                    continue
                return fail(args, report, tail, line, match.group(0), label)
        if _stop:
            break
        time.sleep(args.interval)

    report("[WATCHDOG] stopped for %s; no fatal guest signature seen" % label)
    if status:
        status.close()
    return 0


def fail(args, report, tail, line, matched, label):
    detected_at = datetime.now().astimezone().isoformat()
    report("")
    report("[WATCHDOG][FATAL] %s: machine %d hit '%s'"
           % (label, tail.machine, matched))
    report("[WATCHDOG][FATAL] log: %s" % tail.path)
    report("[WATCHDOG][FATAL] line: %s" % line.rstrip())
    report("[WATCHDOG] last %d line(s) before the failure:" % len(tail.context))
    for ctx in tail.context:
        report("[WATCHDOG]   | %s" % ctx.rstrip())

    tmp = args.flag_file + ".tmp"
    try:
        os.makedirs(os.path.dirname(os.path.abspath(args.flag_file)), exist_ok=True)
        with open(tmp, "w") as fp:
            fp.write("label=%s\n" % label)
            fp.write("machine=%d\n" % tail.machine)
            fp.write("log=%s\n" % tail.path)
            fp.write("matched=%s\n" % matched)
            fp.write("detected_at=%s\n" % detected_at)
            fp.write("line=%s\n" % line.rstrip())
        os.replace(tmp, args.flag_file)
    except OSError as exc:
        report("[WATCHDOG] could not write flag file %s: %s" % (args.flag_file, exc))

    for pid in args.kill_pid:
        try:
            os.kill(int(pid), signal.SIGTERM)
            report("[WATCHDOG] sent SIGTERM to launcher pid %s" % pid)
        except (ValueError, OSError) as exc:
            report("[WATCHDOG] could not signal pid %s: %s" % (pid, exc))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
