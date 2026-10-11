#!/usr/bin/env python3
"""Reproducible local slice/ACV benchmark; never connects to a printer.

Run from the project directory after `cargo build --release`.
Uses GNU time for per-run peak RSS. Artifacts live in an isolated temporary
directory and are removed on exit unless --keep is specified.
"""
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time
import zipfile


def archive_digest(path):
    """Verify CRCs and hash every uncompressed member, ignoring ZIP metadata."""
    digest = hashlib.sha256()
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise RuntimeError("duplicate PWSZ members")
        for name in sorted(names):
            digest.update(name.encode())
            with archive.open(name) as member:
                for chunk in iter(lambda: member.read(1024 * 1024), b""):
                    digest.update(chunk)
    return digest.hexdigest()


def run(binary, model, root, label, supports, threads, command, repetition):
    tag = f"{label}-{supports}-{threads or 'auto'}-{repetition}"
    output = root / f"{tag}.{'pwsz' if command == 'slice' else 'acv'}"
    timings = root / f"{tag}.time"
    environment = dict(os.environ, LC_ALL="C", ANYCUBIC_SLICER_TIMINGS="1",
                       ANYCUBIC_SUPPORT_DIAGNOSTICS="1")
    environment.pop("ANYCUBIC_SLICER_THREADS", None)
    if threads:
        environment["ANYCUBIC_SLICER_THREADS"] = str(threads)
    start = time.perf_counter()
    result = subprocess.run([
        "/usr/bin/time", "-f", "%e %M", "-o", str(timings), str(binary),
        command, str(model), str(output), "--supports", supports,
    ], env=environment, text=True, capture_output=True, check=True)
    elapsed = time.perf_counter() - start
    seconds, rss_kib = timings.read_text().split()
    record = {
        "binary": label, "supports": supports, "threads": threads or "auto",
        "command": command, "run": repetition, "seconds": float(seconds),
        "wall_seconds": elapsed, "peak_rss_mib": int(rss_kib) / 1024,
        "output_bytes": output.stat().st_size, "report": result.stdout.strip(),
        "stages": result.stderr.strip().splitlines(),
    }
    print(json.dumps(record), flush=True)
    return record, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path, nargs="?", default=Path("test/test3.stl"))
    parser.add_argument("--binary", type=Path, default=Path("target/release/anycubicstudio"))
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--threads", type=int, default=0, help="0 = default bounded pool")
    parser.add_argument("--supports", nargs="+", choices=["none", "light", "medium", "heavy"],
                        default=["none", "medium"])
    parser.add_argument("--command", choices=["slice", "viewer-scene"], default="slice")
    parser.add_argument("--verify-determinism", action="store_true",
                        help="compare every PWSZ member with a one-worker run")
    parser.add_argument("--keep", action="store_true", help="keep temporary outputs and logs")
    args = parser.parse_args()
    if args.repeat < 1 or args.threads < 0:
        parser.error("repeat must be positive and threads must be nonnegative")
    if args.verify_determinism and args.command != "slice":
        parser.error("--verify-determinism requires --command slice")
    model = args.model.resolve(strict=True)
    binaries = [("optimized", args.binary.resolve(strict=True))]
    if args.baseline:
        binaries.insert(0, ("baseline", args.baseline.resolve(strict=True)))
    temporary = (nullcontext(tempfile.mkdtemp(prefix="anycubic-bench-")) if args.keep
                 else tempfile.TemporaryDirectory(prefix="anycubic-bench-"))
    with temporary as directory:
        root = Path(directory)
        print(f"Artifacts: {root}", flush=True)
        for label, binary in binaries:
            for preset in args.supports:
                records = []
                for repetition in range(1, args.repeat + 1):
                    record, output = run(binary, model, root, label, preset,
                                         args.threads, args.command, repetition)
                    records.append(record)
                print(json.dumps({"summary": label, "supports": preset,
                                  "median_seconds": statistics.median(r["seconds"] for r in records),
                                  "peak_rss_mib": max(r["peak_rss_mib"] for r in records)}), flush=True)
                if label == "optimized" and args.verify_determinism:
                    _, serial = run(binary, model, root, "verify-serial", preset, 1, "slice", 1)
                    parallel_hash = archive_digest(output)
                    if parallel_hash != archive_digest(serial):
                        raise RuntimeError(f"thread count changed PWSZ content ({preset})")
                    print(f"Verified all members/CRCs, serial == parallel ({preset}): {parallel_hash}", flush=True)


if __name__ == "__main__":
    main()
