#!/usr/bin/env python3
"""Collect every non-Starfish series used by the paper auto-scale figures."""
from __future__ import annotations

import argparse
import fcntl
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PAPER_TIGON_REV = "16f8007fa15bc853397b04e0747efc4f8c21ef25"
MACHINES = [1, 2, 4, 6, 8]
GEMINI_NUMERIC_REL_TOL = 5e-5
GEMINI_HIGH_LATENCY_REL_TOL = 5e-2
TIGON_BRIDGE = "tigon-br0"
TIGON_NETWORK_INTERFACES = frozenset(
    {TIGON_BRIDGE, *(f"tigon-tap{i}" for i in range(8))}
)
TIGON_HOST_LOCK_DIR = Path("/run/lock/starfishos-tigon-host")
TIGON_HOST_LOCK = TIGON_HOST_LOCK_DIR / "runner.lock"
FLOAT_PATTERN = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
GEMINI_SAMPLE_RE = re.compile(
    rf"^exec_time=({FLOAT_PATTERN})\(s\)\r?\n"
    rf"pr_sum=({FLOAT_PATTERN})\r?\n"
    rf"pr\[(\d+)\]=({FLOAT_PATTERN})$",
    re.MULTILINE,
)


def run(cmd, *, cwd=None, log=None, env=None):
    print("+", " ".join(map(str, cmd)), flush=True)
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=1)
    lines = []
    assert proc.stdout is not None
    for line in proc.stdout:
        print(line, end="", flush=True)
        lines.append(line)
    proc.wait()
    output = "".join(lines)
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(output)
    if proc.returncode:
        raise SystemExit(f"command failed ({proc.returncode}); see {log}: {' '.join(map(str, cmd))}")
    return output


class CommandInterrupted(SystemExit):
    """A logged command was stopped by a signal sent to the collector."""


def controller_status(message):
    """Emit a best-effort diagnostic without making cleanup depend on a PTY."""
    try:
        print(message, flush=True)
    except BrokenPipeError:
        # Avoid another EPIPE from Python's interpreter-shutdown flush.
        try:
            sys.stdout = open(os.devnull, "w")
        except OSError:
            pass
    except (OSError, RuntimeError, ValueError):
        pass


def run_to_regular_log(cmd, *, cwd, log, env=None, status_interval=60.0,
                       privileged_process_group=False):
    """Run a verbose command with its output connected directly to ``log``.

    Unlike :func:`run`, this path never pipes child output through the
    collector's controlling PTY and never retains it in memory.  The child has
    its own process group so signals received by the collector can be
    forwarded to the whole command rather than leaving a privileged image
    builder behind.  ``privileged_process_group`` is for a command whose
    descendants become root through sudo; it repeats termination through a
    non-interactive sudo invocation outside the Python signal handler.
    """
    if status_interval <= 0:
        raise ValueError("status_interval must be positive")
    log.parent.mkdir(parents=True, exist_ok=True)
    command = " ".join(map(str, cmd))
    print(f"+ {command}", flush=True)
    print(f"[AE] Verbose output is being written directly to {log}", flush=True)

    interrupted_by = None
    termination_deadline = None
    previous_handlers = {}

    proc = None

    def process_group_exists():
        if proc is None:
            return False
        try:
            os.killpg(proc.pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def signal_process_group(signum, *, include_privileged=False):
        if proc is None:
            return
        try:
            os.killpg(proc.pid, signum)
        except ProcessLookupError:
            pass
        except PermissionError:
            # At least stop the unprivileged command leader if a privileged
            # descendant prevents signalling the whole group.
            try:
                proc.send_signal(signum)
            except (ProcessLookupError, PermissionError):
                pass
        if not include_privileged:
            return

        # The start-vms process group begins with sudo and then consists mostly
        # of root-owned bash/Python/copy/helper processes.  A successful
        # unprivileged killpg only means that it reached at least one permitted
        # member; it does not prove that those privileged descendants stopped.
        # Keep this subprocess call out of forward_signal(): Python signal
        # handlers must not start or wait for another process.
        try:
            result = subprocess.run(
                ["sudo", "-n", "/bin/kill", f"-{signum}", "--",
                 f"-{proc.pid}"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            controller_status(
                f"[AE] WARNING: could not signal privileged process group "
                f"{proc.pid}: {exc}"
            )
            return
        if result.returncode and process_group_exists():
            controller_status(
                f"[AE] WARNING: sudo could not signal privileged process "
                f"group {proc.pid} (exit {result.returncode})"
            )

    def terminate_process_group():
        """Best-effort cleanup for every controller-side error."""
        if proc is None:
            return
        signal_process_group(
            signal.SIGTERM,
            include_privileged=privileged_process_group,
        )
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        except OSError:
            pass
        # The leader may exit before a sudo/nspawn descendant.  Kill any
        # remaining members of its still-unique process group.
        signal_process_group(
            signal.SIGKILL,
            include_privileged=privileged_process_group,
        )
        try:
            proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, OSError):
            pass
        group_deadline = time.monotonic() + 5
        while process_group_exists() and time.monotonic() < group_deadline:
            time.sleep(0.05)

    def forward_signal(signum, _frame):
        nonlocal interrupted_by, termination_deadline
        if interrupted_by is None:
            interrupted_by = signum
            termination_deadline = time.monotonic() + 10.0
        # If Popen has returned, forward immediately.  During the tiny spawn
        # window proc is still None; the post-Popen check below forwards it.
        if not privileged_process_group:
            signal_process_group(signum)

    handled_signals = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    try:
        # Install handlers before Popen creates an independent session.  A
        # signal before spawn prevents the child; one racing with Popen is
        # recorded and forwarded as soon as Popen returns.
        for signum in handled_signals:
            previous_handlers[signum] = signal.signal(signum, forward_signal)
        if interrupted_by is not None:
            raise CommandInterrupted(128 + interrupted_by)

        with log.open("wb") as output:
            if interrupted_by is not None:
                raise CommandInterrupted(128 + interrupted_by)
            proc = subprocess.Popen(
                cmd,
                cwd=cwd,
                env=env,
                stdout=output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            forwarded_interruption = None
            if interrupted_by is not None:
                signal_process_group(
                    interrupted_by,
                    include_privileged=privileged_process_group,
                )
                forwarded_interruption = interrupted_by

            started = time.monotonic()
            next_status = started + status_interval
            while proc.poll() is None:
                now = time.monotonic()
                if (interrupted_by is not None
                        and forwarded_interruption != interrupted_by):
                    signal_process_group(
                        interrupted_by,
                        include_privileged=privileged_process_group,
                    )
                    forwarded_interruption = interrupted_by
                if (interrupted_by is not None
                        and termination_deadline is not None
                        and now >= termination_deadline):
                    controller_status(
                        "[AE] Logged command did not stop after its signal; "
                        "killing its process group"
                    )
                    signal_process_group(
                        signal.SIGKILL,
                        include_privileged=privileged_process_group,
                    )
                    termination_deadline = None
                if now >= next_status:
                    elapsed = int(now - started)
                    try:
                        log_bytes = output.tell()
                    except OSError:
                        log_bytes = -1
                    size = (
                        f"{log_bytes} bytes"
                        if log_bytes >= 0 else "unknown size"
                    )
                    controller_status(
                        f"[AE] Logged command still running after {elapsed}s "
                        f"({size}): {log}"
                    )
                    next_status = now + status_interval
                time.sleep(min(0.2, max(0.0, next_status - now)))
            returncode = proc.wait()
    except BaseException as exc:
        terminate_process_group()
        if process_group_exists():
            raise RuntimeError(
                f"failed to stop logged command process group {proc.pid}; "
                f"see {log}"
            ) from exc
        raise
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)

    if interrupted_by is not None:
        # A command leader can exit on the forwarded signal before a
        # privileged descendant.  Do not leave the rest of its group behind.
        terminate_process_group()
        if process_group_exists():
            raise RuntimeError(
                f"interrupted logged command left process group {proc.pid}; "
                f"see {log}"
            )
        raise CommandInterrupted(128 + interrupted_by)
    if returncode:
        # The command itself may have been signalled directly instead of the
        # collector.  Its leader can report 128+signal while a parallel copy
        # or ivshmem helper remains in the independently created group.
        terminate_process_group()
        if process_group_exists():
            raise RuntimeError(
                f"failed logged command left process group {proc.pid}; "
                f"see {log}"
            )
        raise SystemExit(
            f"command failed ({returncode}); see {log}: {command}"
        )


def require_tools(*names):
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise SystemExit("missing host tools: " + ", ".join(missing))


def average(values, label):
    if not values:
        raise SystemExit(f"could not extract {label}")
    return sum(values) / len(values)


def append_result(path, app, series, n, value, unit):
    with path.open("a") as out:
        out.write(f"\nAE_RESULT app={app} series={series} n={n} value={value:.9f} unit={unit}\n")


def parse_gemini_samples(text, label, expected_samples):
    """Parse complete timing/checksum records without accepting partial runs."""
    samples = [
        (float(seconds), float(pr_sum), int(max_vertex), float(max_value))
        for seconds, pr_sum, max_vertex, max_value
        in GEMINI_SAMPLE_RE.findall(text)
    ]
    if len(samples) != expected_samples:
        raise SystemExit(
            f"expected {expected_samples} complete Gemini samples in {label}, "
            f"found {len(samples)}"
        )
    return samples


def _relative_deviation(value, reference):
    if reference == 0:
        return 0.0 if value == 0 else float("inf")
    return abs(value - reference) / abs(reference)


def analyze_gemini_samples(series, n, samples):
    """Return the measured average and machine-readable quality annotations."""
    # Every Gemini command emits one warmup before its measured samples.
    measured = samples[1:]
    if not measured:
        raise SystemExit("Gemini output contains a warmup but no measured samples")
    reference_time = median(sample[0] for sample in measured)
    reference_sum = median(sample[1] for sample in measured)
    reference_max = median(sample[3] for sample in measured)
    reference_vertex = Counter(sample[2] for sample in measured).most_common(1)[0][0]
    warnings = []
    for sample_index, (seconds, pr_sum, max_vertex, max_value) in enumerate(
        measured, start=1
    ):
        sum_deviation = _relative_deviation(pr_sum, reference_sum)
        max_deviation = _relative_deviation(max_value, reference_max)
        if (sum_deviation > GEMINI_NUMERIC_REL_TOL
                or max_deviation > GEMINI_NUMERIC_REL_TOL
                or max_vertex != reference_vertex):
            warnings.append(
                "AE_WARNING app=gemini "
                f"series={series} n={n} sample={sample_index} "
                "kind=numerical-divergence "
                f"pr_sum_rel_dev={sum_deviation:.9g} "
                f"pr_max_rel_dev={max_deviation:.9g} "
                f"max_vertex={max_vertex} reference_max_vertex={reference_vertex}"
            )
        latency_deviation = _relative_deviation(seconds, reference_time)
        if (seconds > reference_time
                and latency_deviation > GEMINI_HIGH_LATENCY_REL_TOL):
            warnings.append(
                "AE_WARNING app=gemini "
                f"series={series} n={n} sample={sample_index} "
                "kind=high-latency "
                f"exec_time={seconds:.9f} "
                f"exec_time_rel_dev={latency_deviation:.9g}"
            )

    seconds = sum(sample[0] for sample in measured) / len(measured)
    quality = (
        "AE_QUALITY app=gemini "
        f"series={series} n={n} measured={len(measured)} "
        f"status={'warning' if warnings else 'ok'} "
        f"warning_count={len(warnings)}"
    )
    return seconds, warnings, quality


def _append_gemini_annotations(path, warnings, quality, result=None):
    records = ["", *warnings, quality]
    if result is not None:
        series, n, seconds = result
        records.append(
            f"AE_RESULT app=gemini series={series} n={n} "
            f"value={seconds:.9f} unit=s"
        )
    with path.open("a") as out:
        out.write("\n".join(records) + "\n")
    for warning in warnings:
        print(f"[AE] WARNING: {warning}", flush=True)


def append_gemini_result(path, series, n, samples):
    """Record the full-sample average and machine-readable quality warnings."""
    seconds, warnings, quality = analyze_gemini_samples(series, n, samples)
    _append_gemini_annotations(
        path, warnings, quality, result=(series, n, seconds)
    )


def result_exists(path: Path, app: str, series: str, n: int, unit: str) -> bool:
    """Return true only for a complete result matching the requested point."""
    if not path.is_file():
        return False
    marker = re.compile(
        rf"^AE_RESULT app={re.escape(app)} series={re.escape(series)} "
        rf"n={n} value=[-+0-9.eE]+ unit={re.escape(unit)}$",
        re.MULTILINE,
    )
    return marker.search(path.read_text(errors="replace")) is not None


def gemini_result_exists(path: Path, series: str, n: int,
                         expected_samples: int) -> bool:
    """Validate/backfill resumed Gemini results instead of trusting a marker."""
    if not result_exists(path, "gemini", series, n, "s"):
        return False
    text = path.read_text(errors="replace")
    try:
        samples = parse_gemini_samples(text, path.name, expected_samples)
    except SystemExit as exc:
        print(f"[AE] WARNING: {exc}; rerunning {path.name}", flush=True)
        return False

    seconds, warnings, quality = analyze_gemini_samples(series, n, samples)
    values = re.findall(
        rf"^AE_RESULT app=gemini series={re.escape(series)} n={n} "
        rf"value=({FLOAT_PATTERN}) unit=s$",
        text,
        re.MULTILINE,
    )
    tolerance = max(1e-9, abs(seconds) * 1e-12)
    if not values or abs(float(values[-1]) - seconds) > tolerance:
        print(
            f"[AE] WARNING: stale Gemini average in {path.name}; rerunning it",
            flush=True,
        )
        return False

    if quality not in text.splitlines():
        _append_gemini_annotations(path, warnings, quality)
        print(f"[AE] Backfilled sample quality: {path.name}", flush=True)
    return True


def snapshot_generated_files(paths):
    """Capture exact pre-run state for source-tree files produced by builds."""
    snapshots = []
    for path in paths:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            snapshots.append((path, "missing", None, None, None))
            continue
        times = (metadata.st_atime_ns, metadata.st_mtime_ns)
        if stat.S_ISREG(metadata.st_mode):
            snapshots.append((
                path, "file", path.read_bytes(),
                stat.S_IMODE(metadata.st_mode), times,
            ))
        elif stat.S_ISLNK(metadata.st_mode):
            snapshots.append((
                path, "symlink", os.readlink(path),
                stat.S_IMODE(metadata.st_mode), times,
            ))
        else:
            raise SystemExit(
                f"cannot preserve generated path with unsupported type: {path}"
            )
    return snapshots


def _remove_generated_path(path: Path):
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return
    if stat.S_ISDIR(metadata.st_mode):
        shutil.rmtree(path)
    else:
        path.unlink()


def restore_generated_files(snapshots):
    """Restore every snapshot, including files that did not exist before."""
    errors = []
    for path, kind, contents, mode, times in snapshots:
        try:
            if kind == "missing":
                _remove_generated_path(path)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            if kind == "symlink":
                _remove_generated_path(path)
                os.symlink(contents, path)
                if hasattr(os, "lchmod"):
                    os.lchmod(path, mode)
                os.utime(path, ns=times, follow_symlinks=False)
                continue

            temp_name = None
            try:
                with tempfile.NamedTemporaryFile(
                    mode="wb", prefix=f".{path.name}.ae-restore-",
                    dir=path.parent, delete=False,
                ) as restored:
                    temp_name = Path(restored.name)
                    restored.write(contents)
                os.chmod(temp_name, mode)
                try:
                    current = path.lstat()
                except FileNotFoundError:
                    current = None
                if current is not None and stat.S_ISDIR(current.st_mode):
                    shutil.rmtree(path)
                os.replace(temp_name, path)
                temp_name = None
                os.utime(path, ns=times)
            finally:
                if temp_name is not None:
                    temp_name.unlink(missing_ok=True)
        except Exception as exc:  # attempt every restoration before failing
            errors.append(f"{path}: {exc}")
    if errors:
        raise RuntimeError(
            "failed to restore generated source-tree files: " + "; ".join(errors)
        )


def require_disjoint_paths(source: Path, destination: Path):
    """Reject cleanup/build destinations that overlap a source checkout."""
    source_resolved = source.resolve(strict=True)
    destination_resolved = destination.resolve(strict=False)
    if (source_resolved == destination_resolved
            or source_resolved in destination_resolved.parents
            or destination_resolved in source_resolved.parents):
        raise SystemExit(
            "source and generated build tree must be disjoint: "
            f"{source} vs {destination}"
        )


def materialize_git_worktree(source: Path, destination: Path) -> Path:
    """Copy current tracked/nonignored-untracked files into a clean build tree."""
    require_disjoint_paths(source, destination)
    proc = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others",
         "--exclude-standard"],
        cwd=source, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if proc.returncode:
        error = os.fsdecode(proc.stderr).strip()
        raise SystemExit(f"cannot enumerate build inputs in {source}: {error}")

    _remove_generated_path(destination)
    destination.mkdir(parents=True)
    for encoded in proc.stdout.split(b"\0"):
        if not encoded:
            continue
        relative = Path(os.fsdecode(encoded))
        if relative.is_absolute() or ".." in relative.parts:
            raise SystemExit(f"unsafe git worktree path in {source}: {relative}")
        source_path = source / relative
        try:
            metadata = source_path.lstat()
        except FileNotFoundError:
            # git ls-files includes tracked paths deleted in the current tree.
            continue
        destination_path = destination / relative
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        if stat.S_ISREG(metadata.st_mode):
            shutil.copy2(source_path, destination_path)
        elif stat.S_ISLNK(metadata.st_mode):
            os.symlink(os.readlink(source_path), destination_path)
        else:
            raise SystemExit(
                f"unsupported build input type in {source}: {relative}"
            )
    return destination


TIGON_TRANSIENT_IMAGE_ERRORS = re.compile(
    r"(?:Couldn't download packages|Temporary failure resolving|"
    r"Could not resolve|Failed to fetch|Network is unreachable|"
    r"Connection (?:failed|reset|timed out)|Hash Sum mismatch|"
    r"TLS connection was non-properly terminated|HTTP[^\n]*(?:429|502|503|504))",
    re.IGNORECASE,
)


def _prepare_tigon_generated_files(snapshots):
    """Make generated destinations replaceable without following symlinks."""
    for path, kind, _contents, mode, _times in snapshots:
        if kind == "file":
            try:
                path.chmod(mode | stat.S_IWUSR)
            except OSError as exc:
                raise SystemExit(
                    f"cannot make generated Tigon file writable: {path}: {exc}"
                ) from exc
        elif kind == "symlink":
            # cp would follow an existing destination symlink and could overwrite
            # a file outside mkosi.extra.  The original link is restored later.
            _remove_generated_path(path)


def _remove_tigon_refclock_history(path: Path):
    """Remove the upstream append-only line before one fresh append."""
    try:
        contents = path.read_bytes()
    except FileNotFoundError:
        return
    refclock = b"refclock PHC /dev/ptp0 poll 2"
    filtered = b"".join(
        line for line in contents.splitlines(keepends=True)
        if line.rstrip(b"\r\n") != refclock
    )
    path.write_bytes(filtered)


def build_tigon_image(tigon: Path, log_dir: Path, attempts: int = 3):
    """Build Tigon's image, retrying transient downloads without checkout drift."""
    if attempts < 1:
        raise SystemExit("TIGON_IMAGE_ATTEMPTS must be at least 1")

    ssh_dir = tigon / "emulation/image/mkosi.extra/root/.ssh"
    chrony_config = (
        tigon / "emulation/image/mkosi.extra/etc/chrony/chrony.conf"
    )
    generated_snapshots = snapshot_generated_files((
        *(ssh_dir / name for name in
          ("authorized_keys", "id_rsa.pub", "id_rsa", "config")),
        chrony_config,
    ))
    env = os.environ.copy()
    env.setdefault("network_device", "")
    try:
        for attempt in range(1, attempts + 1):
            # Reset partial generated files from the previous attempt first.
            # make_vm_img.sh copies id_rsa as 0400 and appends its PHC refclock
            # line to chrony.conf, so neither path is retry-safe on its own.
            restore_generated_files(generated_snapshots)
            _prepare_tigon_generated_files(generated_snapshots)
            # A checkout can already contain duplicates from older invocations.
            # Preserve them in the final snapshot, but exclude all exact old
            # refclock lines from the image input; upstream appends one fresh
            # canonical line during this attempt.
            _remove_tigon_refclock_history(chrony_config)
            log = log_dir / f"tigon-image-build-attempt{attempt}.log"
            try:
                run_to_regular_log(
                    ["bash", "emulation/image/make_vm_img.sh"],
                    cwd=tigon,
                    log=log,
                    env=env,
                )
                return
            except CommandInterrupted:
                # User/controller signals are not transient mirror failures and
                # must never start another privileged image build.
                raise
            except SystemExit:
                output = log.read_text(errors="replace") if log.is_file() else ""
                if attempt == attempts or not TIGON_TRANSIENT_IMAGE_ERRORS.search(output):
                    raise
                print(
                    f"[AE] Transient Tigon image download failure; retrying "
                    f"({attempt + 1}/{attempts}) with the populated mkosi cache",
                    flush=True,
                )
    finally:
        restore_generated_files(generated_snapshots)


def collect_linux(log_dir: Path, graph: Path):
    expected = []
    for n in MACHINES:
        expected.extend((
            (log_dir / f"matrix_Ideal_N{n}.log", "matrix", "Ideal", n, "us"),
            (log_dir / f"db1000_Ideal_N{n}.log", "db1000", "Ideal", n, "mops"),
            (log_dir / f"gemini_Ideal_N{n}.log", "gemini", "Ideal", n, "s"),
            (log_dir / f"gemini_Distributed_N{n}.log", "gemini", "Distributed", n, "s"),
        ))

    def point_exists(path, app, series, n, unit):
        if app == "gemini":
            expected_samples = 4 if series == "Ideal" else 6
            return gemini_result_exists(path, series, n, expected_samples)
        return result_exists(path, app, series, n, unit)

    if all(point_exists(*point) for point in expected):
        print("[AE] Reusing all completed Linux baseline results", flush=True)
        return

    require_tools("cmake", "git", "make", "mpicxx", "mpirun", "numactl")
    phoenix = ROOT / "test-on-linux/phoenix"
    db_source = ROOT / "test-on-linux/dbx1000"
    gemini = ROOT / "test-on-linux/GeminiGraph"
    distributed = ROOT / "test-on-linux/ggraph-distri"
    for tree in (phoenix, db_source, gemini, distributed):
        if not tree.exists():
            raise SystemExit(f"missing Linux baseline submodule: {tree}")
    if not graph.is_file():
        raise SystemExit(f"missing twitter graph: {graph}; rerun prepare.sh without SKIP_GRAPH_DATASET=1")

    build_root = log_dir.parent / "linux-build"
    phoenix_build = build_root / "phoenix"
    gemini_build = build_root / "gemini"
    db_build = build_root / "dbx1000"
    for source_tree in (phoenix, db_source, gemini, distributed):
        for generated_tree in (
            build_root, phoenix_build, gemini_build, db_build,
        ):
            require_disjoint_paths(source_tree, generated_tree)
    db = materialize_git_worktree(db_source, db_build)
    distributed_pagerank = distributed / "toolkits/pagerank"
    generated_snapshots = snapshot_generated_files((distributed_pagerank,))
    config = db / "config.h"
    try:
        run(["cmake", "-S", str(phoenix), "-B", str(phoenix_build),
             "-DBREAKDOWN=ON",
             "-DCMAKE_C_FLAGS=-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"])
        run(["cmake", "--build", str(phoenix_build),
             "--target", "matrix_multiply", "-j"])
        matrix = phoenix_build / "bin/matrix_multiply"

        run(["cmake", "-S", str(gemini), "-B", str(gemini_build)])
        run(["cmake", "--build", str(gemini_build), "-j"])
        # The upstream target is a direct source-to-binary rule without a
        # configuration stamp.  Force it so an executable from another host,
        # MPI implementation, or compiler cannot be silently reused.
        run(["make", "-B", "-j", "toolkits/pagerank"], cwd=distributed)

        configured = config.read_text()
        configured = re.sub(r"(#define\s+WORKLOAD\s+)\w+", r"\1TPCC", configured)
        configured = re.sub(r"(#define\s+NUM_WH\s+)\d+", r"\g<1>64", configured)
        config.write_text(configured)
        run(["make", "-j"], cwd=db)
        # The Linux target excludes the CHCORE binding-file path, and -c 0
        # makes the existing matrix inputs read-only runtime data.
        matrix_data = phoenix / "data/matrix_datafiles"
        for n in MACHINES:
            app_threads = 8 * n
            graph_threads = 12 * n
            path = log_dir / f"matrix_Ideal_N{n}.log"
            if result_exists(path, "matrix", "Ideal", n, "us"):
                print(f"[AE] Reusing completed result: {path.name}", flush=True)
            else:
                text = run([str(matrix), "-l", "4000", "-r", "4000", "-t", str(app_threads), "-c", "0"],
                           cwd=matrix_data, log=path)
                us = average([float(x) for x in re.findall(r"(?<!inter )library:\s*([\d.]+)", text)], path.name)
                append_result(path, "matrix", "Ideal", n, us, "us")

            path = log_dir / f"db1000_Ideal_N{n}.log"
            if result_exists(path, "db1000", "Ideal", n, "mops"):
                print(f"[AE] Reusing completed result: {path.name}", flush=True)
            else:
                text = run([str(db / "rundb"), f"-t{app_threads}", "-n64", "-Tp0.5"], cwd=db, log=path)
                summaries = re.findall(r"\[summary\]\s+txn_cnt=(\d+).*?run_time=([\d.]+)", text)
                if not summaries:
                    raise SystemExit(f"could not extract DBx1000 summary from {path}")
                txn, runtime = summaries[-1]
                mops = float(txn) / float(runtime) * app_threads / 1e6
                append_result(path, "db1000", "Ideal", n, mops, "mops")

            path = log_dir / f"gemini_Ideal_N{n}.log"
            if gemini_result_exists(path, "Ideal", n, 4):
                print(f"[AE] Reusing completed result: {path.name}", flush=True)
            else:
                text = run(["numactl", "--membind", "0", str(gemini_build / "pagerank"),
                            str(graph), "41652230", "50", str(graph_threads),
                            str(graph_threads)], log=path)
                samples = parse_gemini_samples(text, path.name, 4)
                append_gemini_result(path, "Ideal", n, samples)

            path = log_dir / f"gemini_Distributed_N{n}.log"
            if gemini_result_exists(path, "Distributed", n, 6):
                print(f"[AE] Reusing completed result: {path.name}", flush=True)
            else:
                mpi_env = os.environ.copy()
                mpi_env["OMP_NUM_THREADS"] = "12"
                mpi_env["OMP_PROC_BIND"] = "true"
                text = run(["mpirun", "--map-by", "slot:PE=12", "--bind-to", "core",
                            "-np", str(n), str(distributed / "toolkits/pagerank"),
                            str(graph), "41652230", "50"], log=path, env=mpi_env)
                samples = parse_gemini_samples(text, path.name, 6)
                append_gemini_result(path, "Distributed", n, samples)
    finally:
        restore_generated_files(generated_snapshots)


def collect_matrix_tcp(log_dir: Path):
    pending = [n for n in MACHINES if not result_exists(
        log_dir / f"matrix_Distributed_N{n}.log",
        "matrix", "Distributed", n, "us")]
    if not pending:
        print("[AE] Reusing all completed Matrix TCP results", flush=True)
        return

    require_tools("mpicc", "mpirun")
    binary = log_dir.parent / "matrix_tcp_mpi"
    run(["mpicc", "-O3", "-fopenmp", str(HERE / "matrix_tcp_mpi.c"), "-o", str(binary)])
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = os.environ.get("MATRIX_TCP_THREADS", "8")
    for n in MACHINES:
        path = log_dir / f"matrix_Distributed_N{n}.log"
        if result_exists(path, "matrix", "Distributed", n, "us"):
            print(f"[AE] Reusing completed result: {path.name}", flush=True)
            continue
        text = run(["mpirun", "--bind-to", "none", "--mca", "btl", "tcp,self",
                    "--mca", "btl_tcp_if_include", "lo", "-np", str(n),
                    str(binary), "4000"], log=path, env=env)
        match = re.search(r"TIME=(\d+)", text)
        if not match:
            raise SystemExit(f"could not extract TCP Matrix time from {path}")
        append_result(path, "matrix", "Distributed", n, float(match.group(1)), "us")


def ensure_tigon(path: Path, log_dir: Path):
    """Validate and record the supplied Tigon tree without modifying it."""
    require_tools("git")
    required = ("scripts/run.sh", "scripts/setup.sh",
                "emulation/image/make_vm_img.sh", "emulation/start_vms.sh")
    missing = [name for name in required if not (path / name).is_file()]
    if missing:
        raise SystemExit(
            f"invalid Tigon checkout {path}; missing: {', '.join(missing)}. "
            "Set TIGON_DIR to the existing Tigon source tree."
        )

    def git_output(*args):
        proc = subprocess.run(["git", *args], cwd=path, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode:
            raise SystemExit(f"cannot inspect Tigon checkout {path}: {proc.stderr.strip()}")
        return proc.stdout.strip()

    head = git_output("rev-parse", "HEAD")
    status = git_output("status", "--short")
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", PAPER_TIGON_REV, "HEAD"],
        cwd=path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    metadata = [
        f"tigon_dir={path.resolve()}",
        f"head={head}",
        f"paper_revision={PAPER_TIGON_REV}",
        f"paper_revision_is_ancestor={int(ancestor)}",
        f"worktree_dirty={int(bool(status))}",
    ]
    if status:
        metadata.extend(("", "git_status:", status))
    log_dir.mkdir(parents=True, exist_ok=True)
    (log_dir / "tigon-source.txt").write_text("\n".join(metadata) + "\n")
    print(f"[AE] Using Tigon checkout {path.resolve()} at {head[:12]}", flush=True)
    if not ancestor:
        print(f"[AE] WARNING: paper Tigon revision {PAPER_TIGON_REV[:12]} "
              "is not an ancestor of this checkout", flush=True)
    if status:
        print("[AE] Tigon worktree has local changes; preserving and using them", flush=True)


def _apply_tigon_vm_compat_patch(tigon: Path):
    """Temporarily apply the pinned VM-launch fixes, preserving local edits."""
    patch = HERE / "patches/tigon-vm-compat.patch"
    targets = (
        tigon / "emulation/vm_lib/run_command.py",
        tigon / "emulation/vm_lib/start_vm.py",
    )
    if not patch.is_file():
        raise SystemExit(f"missing Tigon VM compatibility patch: {patch}")

    def check(*extra):
        return subprocess.run(
            ["git", "apply", "--check", *extra, str(patch)],
            cwd=tigon,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    forward = check()
    if forward.returncode == 0:
        snapshots = snapshot_generated_files(targets)
        applied = subprocess.run(
            ["git", "apply", str(patch)],
            cwd=tigon,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if applied.returncode:
            restore_generated_files(snapshots)
            raise SystemExit(
                f"could not apply Tigon VM compatibility patch: "
                f"{applied.stderr.strip()}"
            )
        print("[AE] Temporarily applied Tigon VM compatibility fixes", flush=True)
        return snapshots

    reverse = check("--reverse")
    if reverse.returncode == 0:
        # The supplied checkout already contains the complete fix.  It belongs
        # to the user, so use it as-is and do not reverse it after VM startup.
        print("[AE] Tigon checkout already contains the VM compatibility fixes",
              flush=True)
        return None

    forward_error = forward.stderr.strip() or "forward check failed"
    reverse_error = reverse.stderr.strip() or "reverse check failed"
    raise SystemExit(
        "Tigon VM sources match neither the expected unpatched nor fully "
        "patched form; refusing a partial or ambiguous runtime patch: "
        f"{forward_error}; {reverse_error}"
    )


def _tigon_vm_processes(tigon: Path):
    """Return only QEMU/ivshmem processes owned by this Tigon VM directory."""
    vmdir = (tigon / "emulation/vms").resolve(strict=False)
    ivshmem_socket = (vmdir / "ivshmem_sock").resolve(strict=False)
    matches = set()
    try:
        proc_entries = tuple(Path("/proc").iterdir())
    except OSError:
        return matches

    for entry in proc_entries:
        if not entry.name.isdigit():
            continue
        try:
            raw_args = (entry / "cmdline").read_bytes().split(b"\0")
            args = [os.fsdecode(arg) for arg in raw_args if arg]
            comm = (entry / "comm").read_text().strip()
        except (FileNotFoundError, PermissionError, ProcessLookupError, OSError):
            continue
        if not args:
            continue
        command = Path(args[0]).name

        # Root-owned /proc/<pid>/exe symlinks are not readable by the
        # unprivileged collector on this host.  cmdline and comm remain
        # readable; require both identities plus an exact checkout-owned
        # socket/pidfile argument before a PID is ever passed to sudo kill.
        if command == "ivshmem-server" and comm == "ivshmem-server":
            try:
                socket_arg = args[args.index("--socket-path") + 1]
                socket_path = Path(socket_arg).resolve(strict=False)
            except (ValueError, IndexError, OSError, RuntimeError):
                continue
            if socket_path == ivshmem_socket:
                matches.add(int(entry.name))
            continue

        if not command.startswith("qemu") or not comm.startswith("qemu"):
            continue
        try:
            pidfile_arg = args[args.index("-pidfile") + 1]
            pidfile = Path(pidfile_arg).resolve(strict=False)
            relative = pidfile.relative_to(vmdir)
        except (ValueError, IndexError, OSError, RuntimeError):
            continue
        if (len(relative.parts) == 2 and relative.parts[0].isdigit()
                and relative.parts[1] == "pid"):
            matches.add(int(entry.name))
    return matches


def _cleanup_new_tigon_vm_processes(tigon: Path, processes_before):
    """Stop only detached VM processes created by the failed setup attempt."""
    remaining = _tigon_vm_processes(tigon) - processes_before
    if not remaining:
        return

    def sudo_kill(signum, pids):
        try:
            result = subprocess.run(
                ["sudo", "-n", "/bin/kill", f"-{signum}", "--",
                 *(str(pid) for pid in sorted(pids))],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise RuntimeError(
                f"could not signal failed Tigon VM setup processes: {exc}"
            ) from exc
        return result.returncode

    controller_status(
        "[AE] Stopping detached processes from the failed Tigon VM setup: "
        + ", ".join(map(str, sorted(remaining)))
    )
    term_returncode = sudo_kill(signal.SIGTERM, remaining)
    deadline = time.monotonic() + 10
    while remaining and time.monotonic() < deadline:
        time.sleep(0.1)
        # Revalidate exact command arguments before each subsequent action;
        # never trust a stale pidfile or a PID that could have been reused.
        remaining &= _tigon_vm_processes(tigon)
    if remaining:
        kill_returncode = sudo_kill(signal.SIGKILL, remaining)
        kill_deadline = time.monotonic() + 5
        while remaining and time.monotonic() < kill_deadline:
            time.sleep(0.1)
            remaining &= _tigon_vm_processes(tigon)
    else:
        kill_returncode = 0
    if remaining:
        raise RuntimeError(
            "failed Tigon VM setup processes remain after scoped cleanup: "
            + ", ".join(map(str, sorted(remaining)))
            + f" (TERM exit {term_returncode}, KILL exit {kill_returncode})"
        )


def _mount_records(mountpoint: Path):
    """Read mount ID, filesystem, and source for one exact mountpoint."""
    records = {}
    try:
        lines = Path("/proc/self/mountinfo").read_text().splitlines()
    except OSError as exc:
        raise RuntimeError(f"cannot inspect mount state: {exc}") from exc
    expected = str(mountpoint.resolve(strict=False))
    for line in lines:
        fields = line.split()
        if len(fields) < 8 or fields[4] != expected:
            continue
        try:
            separator = fields.index("-")
            records[fields[0]] = (fields[separator + 1], fields[separator + 2])
        except (ValueError, IndexError):
            continue
    return records


def _mount_ids(mountpoint: Path):
    return frozenset(_mount_records(mountpoint))


def _network_interfaces():
    """Return host interface names without depending on optional pyroute2."""
    try:
        return frozenset(path.name for path in Path("/sys/class/net").iterdir())
    except OSError as exc:
        raise RuntimeError(f"cannot inspect host network interfaces: {exc}") from exc


def _read_ipv4_forwarding():
    try:
        value = Path("/proc/sys/net/ipv4/ip_forward").read_text().strip()
    except OSError as exc:
        raise RuntimeError(f"cannot inspect net.ipv4.ip_forward: {exc}") from exc
    if value not in {"0", "1"}:
        raise RuntimeError(f"unexpected net.ipv4.ip_forward value: {value!r}")
    return value


def _acquire_tigon_host_lock():
    """Serialize fixed-name Tigon network/sysctl state across host users."""
    def sudo_command(command):
        try:
            return subprocess.run(
                ["sudo", "-n", *command],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise SystemExit(f"cannot prepare Tigon host lock: {exc}") from exc

    # /run/lock is sticky and permits user-created entries.  Create a
    # root-owned, non-writable directory first, then refuse any pre-existing
    # object that does not have exactly that safe shape.
    sudo_command(["mkdir", "--", str(TIGON_HOST_LOCK_DIR)])
    try:
        directory = TIGON_HOST_LOCK_DIR.lstat()
    except OSError as exc:
        raise SystemExit(f"cannot inspect Tigon host lock directory: {exc}") from exc
    if (not stat.S_ISDIR(directory.st_mode) or directory.st_uid != 0
            or directory.st_mode & (stat.S_IWGRP | stat.S_IWOTH)):
        raise SystemExit(
            f"refusing unsafe Tigon host lock directory: {TIGON_HOST_LOCK_DIR}"
        )

    touch = sudo_command(["touch", "--", str(TIGON_HOST_LOCK)])
    chmod = sudo_command(["chmod", "0666", "--", str(TIGON_HOST_LOCK)])
    if touch.returncode or chmod.returncode:
        raise SystemExit(
            f"cannot initialize Tigon host lock {TIGON_HOST_LOCK} "
            f"(touch={touch.returncode}, chmod={chmod.returncode})"
        )
    flags = os.O_RDWR | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        lock_fd = os.open(TIGON_HOST_LOCK, flags)
        lock_stat = os.fstat(lock_fd)
        if not stat.S_ISREG(lock_stat.st_mode) or lock_stat.st_uid != 0:
            raise SystemExit(f"refusing unsafe Tigon host lock: {TIGON_HOST_LOCK}")
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        os.close(lock_fd)
        raise SystemExit(
            "another user is already preparing Tigon host network state"
        ) from exc
    except BaseException:
        if "lock_fd" in locals():
            os.close(lock_fd)
        raise
    return lock_fd


def _cleanup_new_tigon_network(interfaces_before, ip_forward_before):
    """Remove only network state created by a failed Tigon start attempt."""
    cleanup_errors = []
    created = ((_network_interfaces() - interfaces_before)
               & TIGON_NETWORK_INTERFACES)
    # Delete enslaved TAPs before their bridge.  Every name was absent at the
    # precondition check, so no pre-existing host interface is eligible here.
    ordered = sorted(created - {TIGON_BRIDGE})
    if TIGON_BRIDGE in created:
        ordered.append(TIGON_BRIDGE)
    for interface in ordered:
        try:
            result = subprocess.run(
                ["sudo", "-n", "ip", "link", "delete", "dev", interface],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            cleanup_errors.append(
                f"could not delete failed Tigon interface {interface}: {exc}"
            )
            continue
        if result.returncode:
            cleanup_errors.append(
                f"failed to delete Tigon interface {interface} "
                f"(exit {result.returncode})"
            )

    try:
        remaining = ((_network_interfaces() - interfaces_before)
                     & TIGON_NETWORK_INTERFACES)
    except RuntimeError as exc:
        cleanup_errors.append(str(exc))
        remaining = created
    if remaining:
        cleanup_errors.append(
            "failed Tigon network interfaces remain: "
            + ", ".join(sorted(remaining))
        )

    # setup_bridge_tap_network only writes 1.  The cross-user Tigon host lock
    # serializes that write with this snapshot and cleanup.
    ip_forward_now = _read_ipv4_forwarding()
    if ip_forward_before == "0" and ip_forward_now == "1":
        try:
            result = subprocess.run(
                ["sudo", "-n", "sysctl", "-q", "-w",
                 "net.ipv4.ip_forward=0"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            cleanup_errors.append(
                f"could not restore net.ipv4.ip_forward: {exc}"
            )
        else:
            try:
                forwarding_restored = (
                    result.returncode == 0 and _read_ipv4_forwarding() == "0"
                )
            except RuntimeError as exc:
                cleanup_errors.append(str(exc))
                forwarding_restored = False
            if not forwarding_restored:
                cleanup_errors.append(
                    "failed to restore net.ipv4.ip_forward=0 "
                    f"(exit {result.returncode})"
                )
    removed = created - remaining
    if removed:
        controller_status(
            "[AE] Removed failed Tigon network interfaces: "
            + ", ".join(interface for interface in ordered
                        if interface in removed)
        )
    if cleanup_errors:
        raise RuntimeError("; ".join(cleanup_errors))


def _cleanup_new_tigon_cxl_mount(tigon: Path, mount_ids_before):
    """Unmount only a CXL tmpfs created by this failed VM-start stage."""
    mountpoint = Path("/mnt/cxl_mem")
    mount_records = _mount_records(mountpoint)
    mount_ids_now = frozenset(mount_records)
    new_mount_ids = mount_ids_now - mount_ids_before
    if not new_mount_ids:
        return
    if len(new_mount_ids) != 1:
        raise RuntimeError(
            f"refusing to unmount ambiguous new mounts at {mountpoint}: "
            f"{sorted(new_mount_ids)}"
        )
    unexpected = {
        mount_id: mount_records[mount_id]
        for mount_id in new_mount_ids
        if mount_records[mount_id] != ("tmpfs", "tmpfs")
    }
    if unexpected:
        raise RuntimeError(
            f"refusing to unmount unexpected filesystem at {mountpoint}: "
            f"{unexpected}"
        )
    active = _tigon_vm_processes(tigon)
    if active:
        raise RuntimeError(
            f"refusing to unmount {mountpoint}; scoped Tigon VM processes "
            f"remain: {', '.join(map(str, sorted(active)))}"
        )
    while new_mount_ids:
        try:
            result = subprocess.run(
                ["sudo", "-n", "umount", "--", str(mountpoint)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise RuntimeError(
                f"could not unmount failed Tigon CXL tmpfs {mountpoint}: {exc}"
            ) from exc
        updated_ids = _mount_ids(mountpoint)
        if result.returncode or updated_ids == mount_ids_now:
            raise RuntimeError(
                f"failed to unmount Tigon CXL tmpfs {mountpoint} "
                f"(exit {result.returncode})"
            )
        mount_ids_now = updated_ids
        new_mount_ids = mount_ids_now - mount_ids_before
    controller_status(f"[AE] Unmounted failed Tigon CXL tmpfs: {mountpoint}")


def _validate_tigon_vm_start_preconditions(
        tigon: Path, mountpoint=Path("/mnt/cxl_mem")):
    """Refuse to replace resources that predate this VM-start attempt."""
    try:
        metadata = mountpoint.lstat()
    except FileNotFoundError:
        metadata = None
    except OSError as exc:
        raise SystemExit(f"cannot inspect Tigon CXL mountpoint: {exc}") from exc
    if metadata is not None and (stat.S_ISLNK(metadata.st_mode)
                                 or not stat.S_ISDIR(metadata.st_mode)):
        raise SystemExit(
            f"refusing Tigon VM startup: {mountpoint} is not a real directory"
        )

    existing_mounts = _mount_records(mountpoint)
    if existing_mounts:
        raise SystemExit(
            f"refusing Tigon VM startup: {mountpoint} is already mounted "
            f"({existing_mounts})"
        )
    existing_processes = _tigon_vm_processes(tigon)
    if existing_processes:
        raise SystemExit(
            "refusing Tigon VM startup: scoped VM processes already exist: "
            + ", ".join(map(str, sorted(existing_processes)))
        )
    try:
        interfaces_before = _network_interfaces()
    except RuntimeError as exc:
        raise SystemExit(str(exc)) from exc
    conflicting_interfaces = interfaces_before & TIGON_NETWORK_INTERFACES
    if conflicting_interfaces:
        raise SystemExit(
            "refusing Tigon VM startup: scoped network interfaces already "
            "exist: " + ", ".join(sorted(conflicting_interfaces))
        )
    try:
        ip_forward_before = _read_ipv4_forwarding()
    except RuntimeError as exc:
        raise SystemExit(str(exc)) from exc
    return (frozenset(existing_processes), frozenset(existing_mounts),
            interfaces_before, ip_forward_before)


def collect_tigon(log_dir: Path, tigon: Path, setup: bool):
    pending = [n for n in MACHINES if not result_exists(
        log_dir / f"db1000_Tigon_N{n}.log", "db1000", "Tigon", n, "mops")]
    if not pending:
        print("[AE] Reusing all completed Tigon results", flush=True)
        return
    ensure_tigon(tigon, log_dir)
    if setup:
        try:
            image_attempts = int(os.environ.get("TIGON_IMAGE_ATTEMPTS", "3"))
        except ValueError as exc:
            raise SystemExit("TIGON_IMAGE_ATTEMPTS must be an integer") from exc
        if image_attempts < 1:
            raise SystemExit("TIGON_IMAGE_ATTEMPTS must be at least 1")
        run(["bash", "scripts/setup.sh", "HOST"], cwd=tigon)
        build_tigon_image(tigon, log_dir, image_attempts)
        tigon_host_lock_fd = _acquire_tigon_host_lock()
        try:
            compatibility_snapshots = _apply_tigon_vm_compat_patch(tigon)
            try:
                (vm_processes_before, cxl_mount_ids_before,
                 interfaces_before,
                 ip_forward_before) = _validate_tigon_vm_start_preconditions(
                     tigon
                 )
                try:
                    run_to_regular_log(
                        ["sudo", "bash", "emulation/start_vms.sh",
                         "--using-old-img", "--cxl", "0", "12", "8", "1",
                         "1"],
                        cwd=tigon,
                        log=log_dir / "tigon-start-vms.log",
                        privileged_process_group=True,
                    )
                    run(["bash", "scripts/setup.sh", "VMS", "8"], cwd=tigon)
                    run(["bash", "scripts/run.sh", "COMPILE_SYNC", "8"],
                        cwd=tigon)
                except BaseException as setup_error:
                    # QEMU -daemonize leaves the command's otherwise unique
                    # process group.  Setup/compile can also fail after all
                    # VMs are live.  In either case clean only resources
                    # created after the locked snapshot, preserving unrelated
                    # host VMs/state.
                    cleanup_errors = []
                    try:
                        _cleanup_new_tigon_vm_processes(
                            tigon, vm_processes_before
                        )
                    except Exception as exc:
                        cleanup_errors.append(str(exc))
                    try:
                        _cleanup_new_tigon_cxl_mount(
                            tigon, cxl_mount_ids_before
                        )
                    except Exception as exc:
                        cleanup_errors.append(str(exc))
                    try:
                        _cleanup_new_tigon_network(
                            interfaces_before, ip_forward_before
                        )
                    except Exception as exc:
                        cleanup_errors.append(str(exc))
                    if cleanup_errors:
                        raise RuntimeError(
                            "Tigon VM setup failed and scoped cleanup was "
                            "incomplete: " + "; ".join(cleanup_errors)
                        ) from setup_error
                    raise
            finally:
                if compatibility_snapshots is not None:
                    restore_generated_files(compatibility_snapshots)
        finally:
            os.close(tigon_host_lock_fd)
    for n in MACHINES:
        path = log_dir / f"db1000_Tigon_N{n}.log"
        if result_exists(path, "db1000", "Tigon", n, "mops"):
            print(f"[AE] Reusing completed result: {path.name}", flush=True)
            continue
        cmd = ["bash", "scripts/run.sh", "TPCC", "TwoPLPasha", str(n), "3",
               "mixed", "10", "15", "1", "0", "1", "Clock", "OnDemand",
               "200000000", "1", "WriteThrough", "None", "30", "10",
               "BLACKHOLE", "0", "0", "0"]
        text = run(cmd, cwd=tigon, log=path)
        vals = [float(x) for x in re.findall(r"Global Stats:.*?total_commit(?:\(TPS\))?:\s*([\d.]+)", text)]
        append_result(path, "db1000", "Tigon", n, average(vals, path.name) / 1e6, "mops")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", required=True, type=Path)
    ap.add_argument("--stages", default="linux,matrix-tcp,tigon")
    ap.add_argument("--graph", type=Path, default=ROOT / "datasets/twitter-2010.bin")
    ap.add_argument("--tigon-dir", type=Path,
                    default=ROOT / "artifact-evaluation/deps/tigon",
                    help="Tigon source tree (default: AE submodule ../deps/tigon)")
    ap.add_argument("--skip-tigon-setup", action="store_true",
                    help="reuse already-running Tigon VMs and synchronized binary")
    args = ap.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    stages = {x.strip() for x in args.stages.split(",") if x.strip()}
    unknown = stages - {"linux", "matrix-tcp", "tigon"}
    if unknown:
        ap.error("unknown stages: " + ", ".join(sorted(unknown)))
    if "linux" in stages:
        collect_linux(args.log_dir, args.graph)
    if "matrix-tcp" in stages:
        collect_matrix_tcp(args.log_dir)
    if "tigon" in stages:
        collect_tigon(args.log_dir, args.tigon_dir, not args.skip_tigon_setup)


if __name__ == "__main__":
    main()
