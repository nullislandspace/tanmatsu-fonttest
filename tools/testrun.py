#!/usr/bin/env python3
"""Drive a PAX font-rendering benchmark run and capture the results.

Connects to the badge's debug console, waits for the app to announce itself,
asks for a run, captures the framed result records until the run ends, and
writes them to results/runs/ as JSON alongside the raw console log.

The device reboots back to the launcher when a run finishes, so capture stops
at the END record rather than waiting for the port to drop.

Stdlib plus pyserial only, matching what the Makefile already depends on.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial (or source the ESP-IDF env)")


# @@BENCH-<KIND>@@ <json> @@<crc32>@@
RECORD = re.compile(r"^@@BENCH-([A-Z]+)@@ (.*) @@([0-9a-f]{8})@@\s*$")

EXIT_OK = 0
EXIT_CONNECTION = 1
EXIT_ABORT = 2
EXIT_UNSTABLE = 3
EXIT_CORRECTNESS = 4


class Capture:
    """Accumulates parsed records and the raw console text of one run."""

    def __init__(self):
        self.records = []
        self.raw = []
        self.corrupt_lines = 0
        self.meta = None
        self.end = None

    def feed(self, line):
        self.raw.append(line)
        match = RECORD.match(line)
        if not match:
            # ESP_LOG noise and anything else: kept in the raw log only.
            return None

        kind, payload, crc_text = match.groups()
        if zlib.crc32(payload.encode()) & 0xFFFFFFFF != int(crc_text, 16):
            # A concurrent log write interleaved into ours. Count it; a run with
            # any corrupt line is not trustworthy.
            self.corrupt_lines += 1
            return None

        try:
            obj = json.loads(payload)
        except json.JSONDecodeError:
            self.corrupt_lines += 1
            return None

        if kind == "BEGIN":
            self.meta = obj
        elif kind == "END":
            self.end = obj
        self.records.append({"kind": kind, "data": obj})
        return kind


def git_info(path):
    """Local git identity, for cross-checking what the firmware reported."""

    def run(*args):
        try:
            out = subprocess.run(
                ["git", "-C", path, *args],
                capture_output=True, text=True, timeout=10,
            )
            return out.stdout.strip() if out.returncode == 0 else None
        except (OSError, subprocess.SubprocessError):
            return None

    head = run("rev-parse", "--short=12", "HEAD")
    if head is None:
        return {"git": "unknown", "dirty": False}
    dirty = bool(run("status", "--porcelain", "--untracked-files=no"))
    return {"git": head, "dirty": dirty}


def connect(url, retries, ready_timeout, delay=2.0):
    """Open the console and wait for the app to answer, reconnecting as needed.

    The badge resets between `make run` and this script, and the proxy holds the
    underlying device open. A connection can therefore be accepted and then
    dropped mid-reboot, so opening the port is not proof of anything -- only a
    PONG or READY record is. Retrying the whole open-and-handshake is what
    removes the need to guess a sleep duration.
    """
    last = None
    for attempt in range(1, retries + 1):
        port = None
        try:
            port = serial.serial_for_url(url, baudrate=115200, timeout=1, do_not_open=True)
            port.open()
            ready = wait_ready(port, min(ready_timeout, 15.0))
            if ready is not None:
                return port, ready
            last = "no READY/PONG"
        except Exception as exc:  # noqa: BLE001 - pyserial raises several types
            last = exc
        if port is not None:
            try:
                port.close()
            except Exception:  # noqa: BLE001
                pass
        print(f"  connect attempt {attempt}/{retries}: {last}", file=sys.stderr)
        time.sleep(delay)
    return None, None


def read_line(port, buffer):
    """Read one newline-terminated line, or None if nothing arrived."""
    chunk = port.read(4096)
    if not chunk:
        return None
    buffer.extend(chunk)
    if b"\n" not in buffer:
        return None
    line, _, rest = bytes(buffer).partition(b"\n")
    buffer.clear()
    buffer.extend(rest)
    return line.decode("utf-8", errors="replace").rstrip("\r")


def wait_ready(port, timeout):
    """Wait for the app to announce itself, prodding it with PING."""
    deadline = time.time() + timeout
    buffer = bytearray()
    last_ping = 0.0

    while time.time() < deadline:
        if time.time() - last_ping >= 1.0:
            port.write(b"PING\n")
            port.flush()
            last_ping = time.time()

        line = read_line(port, buffer)  # raises if the link drops; caller retries
        if line is None:
            continue
        match = RECORD.match(line)
        if match and match.group(1) in ("READY", "PONG"):
            return json.loads(match.group(2))

    return None


def run_benchmark(port, capture, run_timeout, line_timeout):
    port.reset_input_buffer()
    port.write(b"BENCHRUN\n")
    port.flush()

    started = time.time()
    last_line = time.time()
    buffer = bytearray()

    while True:
        now = time.time()
        if now - started > run_timeout:
            return "timeout"
        if now - last_line > line_timeout:
            return "stalled"

        line = read_line(port, buffer)
        if line is None:
            continue
        last_line = now

        kind = capture.feed(line)
        if kind == "END":
            return "ok"
        if kind == "ABORT":
            return "abort"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial URL, e.g. /dev/ttyACM0 or rfc2217://localhost:4001")
    parser.add_argument("--out-dir", default="results")
    parser.add_argument("--label", default="")
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--set-baseline", action="store_true")
    parser.add_argument("--ready-timeout", type=float, default=90.0)
    parser.add_argument("--run-timeout", type=float, default=600.0)
    parser.add_argument("--line-timeout", type=float, default=45.0)
    parser.add_argument("--open-retries", type=int, default=15)
    parser.add_argument("--allow-mismatch", action="store_true")
    args = parser.parse_args()

    print(f"Connecting to {args.port} and waiting for the app to announce itself ...")
    port, ready = connect(args.port, args.open_retries, args.ready_timeout)
    if port is None:
        print("No READY/PONG from the device. Is the benchmark app running?", file=sys.stderr)
        return EXIT_CONNECTION
    print(f"  ready: schema {ready.get('schema')}, {ready.get('cells')} cells, "
          f"opt {ready.get('opt')}, pax {ready.get('pax_git')}")

    print("Running benchmark ...")
    capture = Capture()
    status = run_benchmark(port, capture, args.run_timeout, args.line_timeout)
    try:
        port.close()
    except Exception:  # noqa: BLE001 - the device reboots out from under us
        pass

    # The firmware reports its own verdict in the END record: a run that ended
    # because nobody asked for one is not a result.
    if status == "ok" and capture.end is not None:
        device_status = capture.end.get("status", "ok")
        if device_status != "ok":
            status = device_status

    meta = capture.meta or {}
    build = meta.get("build", {})
    opt = build.get("opt", ready.get("opt", "unknown"))
    pax_git = build.get("pax_git", ready.get("pax_git", "unknown"))

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    parts = [stamp] + ([args.label] if args.label else []) + [opt, pax_git[:8]]
    runid = "-".join(parts)

    failed = status != "ok"
    runs_dir = os.path.join(args.out_dir, "failed" if failed else "runs")
    raw_dir = os.path.join(args.out_dir, "raw")
    os.makedirs(runs_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)

    raw_path = os.path.join(raw_dir, f"{runid}.log")
    with open(raw_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(capture.raw) + "\n")

    local_app = git_info(".")
    local_pax = git_info("components/robotman2412__pax-gfx")
    matches = (
        build.get("app_git", "").startswith(local_app["git"][:8])
        and build.get("pax_git", "").startswith(local_pax["git"][:8])
    )

    cells = [record["data"] for record in capture.records if record["kind"] == "CELL"]
    run = {
        "schema": meta.get("schema"),
        "runid": runid,
        "status": status,
        "meta": meta,
        "end": capture.end,
        "cells": cells,
        "host": {
            "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "port": args.port,
            "app_git": local_app["git"],
            "app_dirty": local_app["dirty"],
            "pax_git": local_pax["git"],
            "pax_dirty": local_pax["dirty"],
            "git_matches_firmware": matches,
            "corrupt_lines": capture.corrupt_lines,
            "records": len(capture.records),
        },
    }

    run_path = os.path.join(runs_dir, f"{runid}.json")
    with open(run_path, "w", encoding="utf-8") as handle:
        json.dump(run, handle, indent=2, sort_keys=True)
        handle.write("\n")

    print(f"  wrote {run_path}")
    print(f"  wrote {raw_path}")
    print(f"  status={status} cells={len(cells)} corrupt_lines={capture.corrupt_lines}")

    if not matches and (build.get("app_git") or build.get("pax_git")):
        print("WARNING: firmware git hashes differ from the local checkout - "
              "the device is running a stale build.", file=sys.stderr)
    if local_app["dirty"] or local_pax["dirty"]:
        print("WARNING: a source tree has uncommitted changes; this run is not reproducible.",
              file=sys.stderr)

    if status == "abort":
        return EXIT_ABORT
    if status != "ok":
        return EXIT_CONNECTION

    if args.set_baseline:
        baseline_path = os.path.join(args.out_dir, f"baseline-{opt.lower()}.json")
        with open(baseline_path, "w", encoding="utf-8") as handle:
            json.dump(run, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print(f"  wrote {baseline_path} (new baseline)")

    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
