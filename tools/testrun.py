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
import base64
import datetime
import json
import os
import re
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_png  # noqa: E402  - local module, path set above

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial (or source the ESP-IDF env)")


# @@BENCH-<KIND>@@ <json> @@<crc32>@@
#
# The payload is non-greedy on purpose. If the device ever drops bytes mid-line,
# a truncated record and the next whole one arrive spliced together with no
# newline between them; a greedy payload would span both and yield JSON that
# fails to parse at the splice. Non-greedy stops at the first complete record,
# and the CRC then rejects the truncated half.
RECORD = re.compile(r"^@@BENCH-([A-Z]+)@@ (.*?) @@([0-9a-f]{8})@@\s*$")

EXIT_OK = 0
EXIT_CONNECTION = 1
EXIT_ABORT = 2
EXIT_UNSTABLE = 3
EXIT_CORRECTNESS = 4
EXIT_USAGE = 5


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


def fnv1a(data):
    """Mirror of bench_fnv1a() in the firmware, so a dump can be verified."""
    h = 0x811C9DC5
    for byte in data:
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def parse_record(line):
    """Return (kind, obj) for a valid framed record, or (None, None)."""
    match = RECORD.match(line)
    if not match:
        return None, None
    kind, payload, crc_text = match.groups()
    if zlib.crc32(payload.encode()) & 0xFFFFFFFF != int(crc_text, 16):
        return None, None
    try:
        return kind, json.loads(payload)
    except json.JSONDecodeError:
        return None, None


def dump_fb(port, cell_id, timeout=60.0):
    """Ask the device for one cell's framebuffer and return (meta, pixels).

    Raises on a chunk that never arrives or a hash that disagrees: a silently
    incomplete reference would bless whatever it happened to capture.
    """
    port.reset_input_buffer()
    port.write(f"DUMPFB {cell_id}\n".encode())
    port.flush()

    buffer = bytearray()
    chunks = {}
    meta = None
    deadline = time.time() + timeout

    while time.time() < deadline:
        line = read_line(port, buffer)
        if line is None:
            continue
        kind, obj = parse_record(line)
        if kind == "FBBEGIN":
            meta = obj
        elif kind == "FB":
            chunks[obj["c"]] = obj["d"]
        elif kind == "ABORT":
            raise RuntimeError(f"device aborted the dump: {obj}")
        elif kind == "FBEND":
            break
    else:
        raise TimeoutError(f"no FBEND for {cell_id}")

    if meta is None:
        raise RuntimeError(f"no FBBEGIN for {cell_id}")
    missing = [i for i in range(meta["chunks"]) if i not in chunks]
    if missing:
        raise RuntimeError(f"{cell_id}: {len(missing)} chunks missing, first {missing[0]}")

    pixels = base64.b64decode("".join(chunks[i] for i in range(meta["chunks"])))
    if len(pixels) != meta["bytes"]:
        raise RuntimeError(f"{cell_id}: got {len(pixels)} bytes, expected {meta['bytes']}")
    if f"{fnv1a(pixels):08x}" != meta["h"]:
        raise RuntimeError(f"{cell_id}: hash mismatch on reassembly")
    return meta, pixels


def capture_refs(port, cell_ids, out_dir, run_meta):
    """Dump every named cell and write results/refs/<id>.png plus a manifest."""
    refs_dir = os.path.join(out_dir, "refs")
    os.makedirs(refs_dir, exist_ok=True)
    manifest_path = os.path.join(refs_dir, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as handle:
            manifest = json.load(handle)

    build = run_meta.get("build", {})
    failures = []
    for index, cell_id in enumerate(cell_ids, 1):
        try:
            meta, pixels = dump_fb(port, cell_id)
        except Exception as exc:  # noqa: BLE001 - one bad cell must not lose the rest
            print(f"  [{index}/{len(cell_ids)}] {cell_id}: FAILED ({exc})", file=sys.stderr)
            failures.append(cell_id)
            continue

        rgb = bench_png.to_rgb(pixels, meta["fmt"], meta["tile"], meta["tile"])
        bench_png.write_png(os.path.join(refs_dir, f"{cell_id}.png"), meta["tile"], meta["tile"], rgb)
        manifest[cell_id] = {
            "hash": meta["h"],
            "fmt": meta["fmt"],
            "orient": meta["orient"],
            "tile": meta["tile"],
            "bytes": meta["bytes"],
            "pax_git": build.get("pax_git"),
            "opt": build.get("opt"),
            "corpus_ver": run_meta.get("bench", {}).get("corpus_ver"),
            "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "blessed": manifest.get(cell_id, {}).get("blessed", []),
        }
        print(f"  [{index}/{len(cell_ids)}] {cell_id}: {meta['bytes']} bytes")

    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"  wrote {manifest_path}")
    return failures


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
    """Return one complete line, or None if none is available yet.

    Two details here are load-bearing, and getting either wrong makes the device
    look like it hangs mid-run:

    * The buffer is drained before the port is read again. One read routinely
      carries several records, and going back to the port before parsing them
      caps the host at roughly one line per read.
    * The read asks for what is actually waiting, not a fixed 4096. pyserial
      blocks until the requested count arrives or the timeout expires, so asking
      for a full buffer costs a whole second per call whenever the device sends
      less than that -- which is every cell.

    Together those made the host consume lines slower than the firmware produced
    them. The backlog filled the link, the badge's USB-CDC FIFO filled behind it,
    and the firmware then silently dropped its own output while the run carried
    on to completion off-screen.
    """
    if b"\n" not in buffer:
        chunk = port.read(max(1, port.in_waiting))
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
            try:
                return json.loads(match.group(2))
            except json.JSONDecodeError:
                # One damaged banner is not a reason to abandon the attempt:
                # another one follows every two seconds.
                continue

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


def reference_cell_ids(args):
    """Cell ids to capture references for: the baseline's, else the newest run's."""
    candidates = []
    if args.baseline and os.path.exists(args.baseline):
        candidates.append(args.baseline)
    runs_dir = os.path.join(args.out_dir, "runs")
    if os.path.isdir(runs_dir):
        names = sorted(n for n in os.listdir(runs_dir) if n.endswith(".json"))
        if names:
            candidates.append(os.path.join(runs_dir, names[-1]))

    for path in candidates:
        with open(path, encoding="utf-8") as handle:
            run = json.load(handle)
        ids = [c["id"] for c in run.get("cells", [])]
        if ids:
            return ids
    return []


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
    parser.add_argument("--capture-refs", action="store_true",
                        help="dump every cell's framebuffer to results/refs/ instead of running "
                             "the matrix; do this once, at baseline time")
    parser.add_argument("--cells", default=None,
                        help="comma-separated cell ids for --capture-refs (default: every cell "
                             "in the baseline, or in the newest run)")
    args = parser.parse_args()

    print(f"Connecting to {args.port} and waiting for the app to announce itself ...")
    port, ready = connect(args.port, args.open_retries, args.ready_timeout)
    if port is None:
        print("No READY/PONG from the device. Is the benchmark app running?", file=sys.stderr)
        return EXIT_CONNECTION
    print(f"  ready: schema {ready.get('schema')}, {ready.get('cells')} cells, "
          f"opt {ready.get('opt')}, pax {ready.get('pax_git')}")

    if args.capture_refs:
        if args.cells:
            cell_ids = [c.strip() for c in args.cells.split(",") if c.strip()]
        else:
            cell_ids = reference_cell_ids(args)
        if not cell_ids:
            print("No cell list available. Capture a run first, or pass --cells.", file=sys.stderr)
            return EXIT_USAGE
        print(f"Capturing {len(cell_ids)} reference framebuffers ...")
        # PONG carries the identity fields, which is enough to stamp the
        # manifest; a full BEGIN record only exists inside a run.
        failures = capture_refs(port, cell_ids, args.out_dir, {"build": ready, "bench": ready})
        try:
            port.close()
        except Exception:  # noqa: BLE001
            pass
        if failures:
            print(f"{len(failures)} cells failed to dump", file=sys.stderr)
            return EXIT_CONNECTION
        return EXIT_OK

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
