# SPDX-License-Identifier: GPL-3.0-or-later
"""Small on-disk B2 integration baseline (stdlib); no search or representative-corpus claim."""
import argparse
from datetime import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
MODES = ["store", "zstd3", "zstd6", "lzma6", "b-base", "b-global", "b-file", "b-cleared"]


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def snapshot(root):
    return {p.relative_to(root).as_posix(): {"bytes": p.stat().st_size, "sha256": digest(p)}
            for p in sorted(root.rglob("*")) if p.is_file()}


def same_files(left, right):
    if snapshot(left) != snapshot(right):
        return False
    for p in left.rglob("*"):
        if p.is_file():
            with p.open("rb") as a, (right / p.relative_to(left)).open("rb") as b:
                while True:
                    x, y = a.read(65536), b.read(65536)
                    if x != y:
                        return False
                    if not x:
                        break
    return True


def invoke(command, log):
    start = time.perf_counter()
    try:
        result = subprocess.run(command, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b"") + b"\nTIMEOUT\n" + (error.stderr or b""))
        raise RuntimeError(f"timeout: {command}") from error
    elapsed = time.perf_counter() - start
    log.write_bytes(result.stdout + b"\n--- stderr ---\n" + result.stderr)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {log}")
    data = json.loads(result.stdout)
    if not isinstance(data.get("peak_working_set_bytes"), int) or data["peak_working_set_bytes"] <= 0:
        raise RuntimeError("peak working set measurement unavailable")
    return data, elapsed


def validate_plan(result, mode, target):
    assert result["schema"] == "seedarc.b2-compression.r3" and result["mode"] == mode
    files = {f["file_id"]: f["path"] for f in result["files"]}
    for i, b in enumerate(result["blocks"]):
        assert b["block_index"] == b["block_id"] == i
        assert b["file_id"] in files
        codec, config = b["planned_codec"], b["planned_config"]
        if mode in ("store", "zstd3", "zstd6", "lzma6"):
            assert (codec, config) == {"store": (0, 0), "zstd3": (1, 3),
                                       "zstd6": (1, 6), "lzma6": (2, 6)}[mode]
        else:
            assert codec in (0, 1, 2)
            level = 3 if mode == "b-base" else 4
            if mode == "b-file" and files[b["file_id"]] == target:
                level = 6
            assert config == {0: 0, 1: level, 2: 6}[codec]
        if b["actual_codec"] == 0:
            assert b["actual_config"] == 0 and b["stored_bytes"] == b["raw_bytes"]
        else:
            assert (b["actual_codec"], b["actual_config"]) == (codec, config)
            assert 0 < b["stored_bytes"] < b["raw_bytes"]


def summarize(raw):
    rows = []
    for dataset in sorted({r["dataset"] for r in raw}):
        for mode in MODES:
            group = [r for r in raw if r["dataset"] == dataset and r["mode"] == mode]
            assert group and all(r["status"] == "passed" and r["byte_exact"] for r in group)
            hashes = {r["archive_sha256"] for r in group}
            assert len(hashes) == 1, "same-environment archive nondeterminism"
            rows.append({"dataset": dataset, "mode": mode, "archive_bytes": group[0]["archive_bytes"],
                         "repetitions": len(group), "deterministic": True,
                         "fallback_blocks": sum(b["planned_codec"] != 0 and b["actual_codec"] == 0
                                                for b in group[0]["plan"]["blocks"]),
                         **{f"median_{op}_seconds": statistics.median(r[f"{op}_seconds"] for r in group)
                            for op in ("compress", "verify", "extract")},
                         **{f"max_{op}_peak_working_set_bytes": max(r[f"{op}_peak_working_set_bytes"] for r in group)
                            for op in ("compress", "verify", "extract")}})
        for repeat in sorted({r["repeat"] for r in raw}):
            group = {r["mode"]: r for r in raw if r["dataset"] == dataset and r["repeat"] == repeat}
            assert group["b-global"]["archive_sha256"] == group["b-cleared"]["archive_sha256"]
            plans = [group[m]["plan"]["blocks"] for m in ("b-base", "b-global", "b-file", "b-cleared")]
            assert all(len(p) == len(plans[0]) for p in plans)
            assert all(group[m]["plan"]["files"] == group["b-base"]["plan"]["files"]
                       for m in ("b-global", "b-file", "b-cleared"))
            assert all(a["planned_codec"] == b["planned_codec"] == c["planned_codec"] == d["planned_codec"]
                       for a, b, c, d in zip(*plans))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if (not 1 <= args.repetitions <= 10 or not args.run_id or len(args.run_id) > 100
            or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.run_id)):
        parser.error("invalid run ID or repetition count")
    run, local = ROOT / "research/experiments" / args.run_id, ROOT / "research/local" / args.run_id
    # A coordinator may pre-register record.md in the run directory; preserve it.
    if local.exists() or (run.exists() and any(p.name != "record.md" for p in run.iterdir())):
        parser.error("run exists; use a new run ID")
    source_tool = args.tool.resolve(strict=True)
    run.mkdir(parents=True, exist_ok=True)
    local.mkdir(parents=True)
    tool = local / source_tool.name
    shutil.copyfile(source_tool, tool)
    shutil.copyfile(__file__, run / "b2_compression.py")
    config = {"status": "running", "run_id": args.run_id, "repetitions": args.repetitions,
              "modes": MODES, "chunk_bytes": 65536, "candidate": {"id": 2, "revision": 1},
              "catalog_revision": 1, "overlay_revision": 1, "codec_seed": 0,
              "threads": 1, "candidate_count": 1, "search_seconds": None,
              "archive_strategy_version": 0, "archive_seed_length": 0,
              "build": "caller-supplied MSVC Release; see build evidence",
              "timing": "process wall time incl startup, planning, round-trip self-check and JSON evidence",
              "memory": "GetProcessMemoryInfo PeakWorkingSetSize per operation process; includes tool/report",
              "cache": "no warmup; uncontrolled OS caches; rotate mode order each repetition",
              "usage": "exploratory integration baseline; no held-out corpus, tuning or search",
              "tool_sha256": digest(tool), "script_sha256": digest(Path(__file__)),
              "started_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
              "git_commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
              "dirty": True, "os": platform.platform(), "cpu": platform.processor(),
              "logical_cpus": os.cpu_count(), "python": sys.version, "invocation": [sys.executable, *sys.argv],
              "codec_versions": subprocess.check_output([str(tool), "--version"], text=True).strip()}
    save(run / "config.json", config)
    text = (b"SeedArc on-disk compression baseline; explicit plans and byte exact recovery.\n" * 6000)[:393216]
    noise = b"".join(hashlib.sha256(b"seedarc-b2-r3:" + i.to_bytes(8, "little")).digest() for i in range(12288))
    source_names = ["core/checked_arithmetic.cpp", "core/store_archive.cpp", "core/version.cpp"]
    sources = {Path(p).name: (ROOT / p).read_bytes() for p in source_names}
    datasets = {"text": ({"text.txt": text}, "text.txt"),
                "source": (sources, "store_archive.cpp"),
                "binary": ({"binary.bin": noise}, "binary.bin"),
                "compressed": ({"packed.gz": gzip.compress(noise, compresslevel=6, mtime=0)}, "packed.gz"),
                "mixed": ({"000-text.txt": text, "sub/binary.bin": noise,
                           "sub/packed.gz": gzip.compress(text, mtime=0), "sub/empty": b"",
                           "unicode/\u4e2d\u6587.txt": b"SeedArc Unicode fixture\n"}, "000-text.txt")}
    manifest = {"id": "b2-on-disk-r3", "source": "project fixtures plus actual project source files",
                "license": "GPL-3.0-or-later; generated fixture/original project source",
                "source_files": {p: digest(ROOT / p) for p in source_names}, "datasets": {}}
    for name, (files, target) in datasets.items():
        directory = local / "inputs" / name
        for relative, data in files.items():
            p = directory / relative; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
        manifest["datasets"][name] = {"files": snapshot(directory), "target": target,
            "selection_reason": "preselected primary file; no compression-guided selection"}
    save(run / "datasets.json", manifest)
    config["dataset_manifest_sha256"] = digest(run / "datasets.json")
    save(run / "config.json", config)
    raw = []
    try:
        for repeat in range(args.repetitions):
            order = MODES[repeat:] + MODES[:repeat]
            for dataset, (_, target) in datasets.items():
                for mode in order:
                    label = f"{repeat}-{dataset}-{mode}"
                    archive, out = local / f"{label}.sarc", local / f"{label}-out"
                    inp = local / "inputs" / dataset
                    commands = [[str(tool), "compress", mode, str(inp), str(archive), target if mode == "b-file" else "-"],
                                [str(tool), "verify", str(archive)], [str(tool), "extract", str(archive), str(out)]]
                    row = {"repeat": repeat, "dataset": dataset, "mode": mode, "status": "running",
                           "commands": commands, "candidate_count": 1, "search_seconds": None}
                    raw.append(row)
                    try:
                        for op, command in zip(("compress", "verify", "extract"), commands):
                            result, elapsed = invoke(command, run / f"{label}-{op}.log")
                            row[f"{op}_seconds"] = elapsed
                            row[f"{op}_peak_working_set_bytes"] = result["peak_working_set_bytes"]
                            if op == "compress":
                                validate_plan(result, mode, target); row["plan"] = result
                        row["byte_exact"] = same_files(inp, out)
                        assert row["byte_exact"], "round-trip mismatch"
                        row["archive_bytes"] = archive.stat().st_size
                        assert row["archive_bytes"] == row["plan"]["archive_bytes"]
                        row["archive_sha256"] = digest(archive)
                        row["status"] = "passed"
                    except Exception as error:
                        row["status"], row["error"] = "failed", str(error)
                        raise
                    finally:
                        save(run / "raw.json", raw)
                    print(label, row["archive_bytes"], "byte-exact", flush=True)
        save(run / "summary.json", summarize(raw))
        config["status"] = "completed"
    except Exception as error:
        config["status"], config["error"] = "failed", str(error)
        raise
    finally:
        config["finished_at"] = datetime.now(ZoneInfo("America/Los_Angeles")).isoformat()
        save(run / "config.json", config)


if __name__ == "__main__":
    main()
