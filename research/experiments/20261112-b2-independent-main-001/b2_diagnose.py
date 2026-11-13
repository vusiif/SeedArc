# SPDX-License-Identifier: GPL-3.0-or-later
"""B2-r4 P0: per-block diagnostics and simple allocation baselines (stdlib only).

Reuses the B2-r3 on-disk fixtures (byte-identical, checked by hash) and the same 64 KiB block
boundaries. Runs every mode with compress -> verify -> extract and byte-exact comparison, then
exports one machine-readable row per actual block and derives the summaries/attribution from them.
No seed search, no new seed encoding; B2-file-codec is an encode-side oracle over 3 fixed configs.

  python research/scripts/b2_diagnose.py --tool TOOL --run-id RUN [--repetitions 3]
  python research/scripts/b2_diagnose.py --analyze-only research/experiments/RUN
"""
import argparse
import csv
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
R3_RUN = ROOT / "research/experiments/20261112-b2-compression-main-001"
SCHEMA = "seedarc.b2-compression.r4"
HEADER_BYTES = 96
FIXED = {"store": (0, 0), "zstd3": (1, 3), "zstd6": (1, 6), "lzma6": (2, 6)}
B_MODES = ["b-base", "b-global", "b-file", "b-cleared"]
DIRECT_MODES = [*FIXED, *B_MODES]
PLAN_MODES = ["b2-file-codec-c", "b2-file-codec"]  # codec only; codec + file-level level override
MODES = DIRECT_MODES + PLAN_MODES
# P0-B labels: B0 is B2-r3's b-global (the 66,538 B / +16.94 % comparison point).
LABELS = {"b-global": "B0-current", "zstd3": "B1-fixed-zstd3", "b2-file-codec": "B2-file-codec",
          "b2-file-codec-c": "B2-file-codec (codec only)"}
CODEC = {0: "store", 1: "zstd", 2: "lzma2"}
CSV_FIELDS = ["dataset_id", "mode", "strategy", "file_key", "file_id", "file_block_index",
              "block_index", "block_id", "raw_bytes", "planned_codec", "planned_level",
              "actual_codec", "actual_level", "candidate_status", "candidate_encoded_bytes",
              "stored_bytes", "fallback_to_store", "fallback_reason", "store_cause", "plan_source"]


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
            if p.read_bytes() != (right / p.relative_to(left)).read_bytes():
                return False
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


def fixtures():
    """The B2-r3 inputs, rebuilt byte-identically; source files come from the r3 snapshot."""
    r3 = json.loads((R3_RUN / "datasets.json").read_text(encoding="utf-8"))
    sources = {}
    for relative, expected in r3["source_files"].items():
        path = R3_RUN / "dataset-source" / Path(relative).name
        if digest(path) != expected:
            raise RuntimeError(f"r3 dataset source changed: {path}")
        sources[path.name] = path.read_bytes()
    text = (b"SeedArc on-disk compression baseline; explicit plans and byte exact recovery.\n" * 6000)[:393216]
    noise = b"".join(hashlib.sha256(b"seedarc-b2-r3:" + i.to_bytes(8, "little")).digest() for i in range(12288))
    datasets = {"text": ({"text.txt": text}, "text.txt"),
                "source": (sources, "store_archive.cpp"),
                "binary": ({"binary.bin": noise}, "binary.bin"),
                "compressed": ({"packed.gz": gzip.compress(noise, compresslevel=6, mtime=0)}, "packed.gz"),
                "mixed": ({"000-text.txt": text, "sub/binary.bin": noise,
                           "sub/packed.gz": gzip.compress(text, mtime=0), "sub/empty": b"",
                           "unicode/中文.txt": b"SeedArc Unicode fixture\n"}, "000-text.txt")}
    return datasets, r3


def validate(result, mode, target):
    assert result["schema"] == SCHEMA and result["mode"] == ("file-plan" if mode in PLAN_MODES else mode)
    assert result["header_bytes"] == HEADER_BYTES
    assert result["strategy_version"] == 0 and result["seed_length"] == 0
    blocks = result["blocks"]
    assert result["block_count"] == len(blocks)
    assert sum(b["stored_bytes"] for b in blocks) == result["data_bytes"]
    assert HEADER_BYTES + result["data_bytes"] + result["index_bytes"] == result["archive_bytes"]
    files = {f["file_id"]: f["path"] for f in result["files"]}
    for i, b in enumerate(blocks):
        assert b["block_index"] == b["block_id"] == i and files[b["file_id"]] == b["file_key"]
        codec, config = b["planned_codec"], b["planned_config"]
        if mode in FIXED:
            assert (codec, config) == FIXED[mode] and b["plan_source"] == "fixed"
        elif mode in B_MODES:
            override = mode == "b-file" and b["file_key"] == target and codec == 1
            level = 3 if mode == "b-base" else 6 if override else 4
            assert config == {0: 0, 1: level, 2: 6}[codec]
            assert b["plan_source"] == ("base" if mode == "b-base" else
                                        "file_override" if override else "global")
        else:
            assert b["plan_source"] in ("file_codec", "file_override")
        if b["actual_codec"] == 0:
            assert b["actual_config"] == 0 and b["stored_bytes"] == b["raw_bytes"]
        else:
            assert (b["actual_codec"], b["actual_config"]) == (codec, config)
            assert 0 < b["stored_bytes"] < b["raw_bytes"]


def file_codec_plans(results):
    """B2-file-codec from same-boundary fixed runs: per file pick zstd3 vs lzma6 by total stored
    bytes (tie -> zstd3), then a file-level Zstd level override 3 -> 6 only when it is smaller.
    Store is implicit through the core's per-block fallback. Returns {mode: {path: (src, c, l)}}."""
    per = {m: {} for m in ("zstd3", "zstd6", "lzma6")}
    for m in per:
        for b in results[m]["blocks"]:
            per[m][b["file_key"]] = per[m].get(b["file_key"], 0) + b["stored_bytes"]
    paths = [f["path"] for f in results["zstd3"]["files"]]
    codec_only, final, trials = {}, {}, {}
    for p in paths:
        z3, z6, l6 = (per[m].get(p, 0) for m in ("zstd3", "zstd6", "lzma6"))
        trials[p] = {"zstd3": z3, "zstd6": z6, "lzma6": l6}
        codec_only[p] = ("file_codec", 2, 6) if l6 < z3 else ("file_codec", 1, 3)
        final[p] = (("file_override", 1, 6) if codec_only[p][1] == 1 and z6 < z3
                    else codec_only[p])
    return {"b2-file-codec-c": codec_only, "b2-file-codec": final}, trials


def write_plan(path, plan):
    lines = [f"{src} {codec} {level} {p}\n" for p, (src, codec, level) in sorted(plan.items())]
    path.write_bytes("".join(lines).encode("utf-8"))


# ---------------------------------------------------------------- analysis (raw.json only)

def block_rows(dataset, mode, result):
    rows = []
    for b in result["blocks"]:
        planned, actual = b["planned_codec"], b["actual_codec"]
        fallback = planned != 0 and actual == 0
        rows.append({
            "dataset_id": dataset, "mode": mode, "strategy": LABELS.get(mode, mode),
            "file_key": b["file_key"], "file_id": b["file_id"],
            "file_block_index": b["file_block_index"], "block_index": b["block_index"],
            "block_id": b["block_id"], "raw_bytes": b["raw_bytes"],
            "planned_codec": CODEC[planned], "planned_level": b["planned_config"],
            "actual_codec": CODEC[actual], "actual_level": b["actual_config"],
            # The core only learns a candidate length when it is strictly smaller than raw.
            "candidate_status": ("not_attempted" if planned == 0 else
                                 "rejected_not_smaller" if fallback else "accepted"),
            "candidate_encoded_bytes": None if planned == 0 or fallback else b["stored_bytes"],
            "stored_bytes": b["stored_bytes"], "fallback_to_store": fallback,
            "fallback_reason": "no_space_gain" if fallback else None,
            "store_cause": ("planned_store" if planned == 0 else
                            "fallback_no_space_gain" if fallback else None),
            "plan_source": b["plan_source"]})
    return rows


def group(rows, key):
    out = {}
    for r in rows:
        k = key(r)
        g = out.setdefault(k, {"blocks": 0, "raw_bytes": 0, "stored_bytes": 0})
        g["blocks"] += 1
        g["raw_bytes"] += r["raw_bytes"]
        g["stored_bytes"] += r["stored_bytes"]
    return {k if isinstance(k, str) else "/".join(map(str, k)): v for k, v in sorted(out.items(), key=str)}


def category(b0, ref):
    """Why a B0 block differs from the same block under the reference strategy."""
    if b0["store_cause"] == "planned_store":
        return "b0_planned_store"
    if b0["fallback_to_store"]:
        return "b0_fallback_store"
    return f"b0_{b0['actual_codec']}{b0['actual_level']}_vs_{ref['actual_codec']}{ref['actual_level'] if ref['actual_codec'] != 'store' else ''}"


def attribution(rows_by, dataset, b0_mode, ref_mode):
    b0, ref = rows_by[(dataset, b0_mode)], rows_by[(dataset, ref_mode)]
    cats = {}
    for x, y in zip(b0, ref, strict=True):
        assert (x["file_key"], x["file_block_index"], x["raw_bytes"]) == \
               (y["file_key"], y["file_block_index"], y["raw_bytes"])
        delta = x["stored_bytes"] - y["stored_bytes"]
        c = cats.setdefault(category(x, y), {"blocks": 0, "delta_bytes": 0, "block_indexes": []})
        c["blocks"] += 1
        c["delta_bytes"] += delta
        c["block_indexes"].append(x["block_index"])
    return cats


def analyze(run):
    raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
    datasets = sorted({r["dataset"] for r in raw})
    first = {}
    for r in raw:
        assert r["status"] == "passed" and r["byte_exact"]
        first.setdefault((r["dataset"], r["mode"]), r)
    determinism = {}
    for (d, m), r0 in first.items():
        group_rows = [r for r in raw if r["dataset"] == d and r["mode"] == m]
        determinism[f"{d}/{m}"] = len({r["archive_sha256"] for r in group_rows}) == 1
        assert determinism[f"{d}/{m}"], "same-environment archive nondeterminism"
    rows_by = {(d, m): block_rows(d, m, first[(d, m)]["result"]) for d in datasets for m in MODES}

    # Identical block boundaries across every strategy of a dataset (no boundary confound).
    boundaries = {}
    for d in datasets:
        ref = [(r["file_key"], r["file_block_index"], r["block_id"], r["raw_bytes"])
               for r in rows_by[(d, "store")]]
        boundaries[d] = all(ref == [(r["file_key"], r["file_block_index"], r["block_id"], r["raw_bytes"])
                                    for r in rows_by[(d, m)]] for m in MODES)
        assert boundaries[d], f"block boundaries differ in {d}"

    all_rows = [row for d in datasets for m in MODES for row in rows_by[(d, m)]]
    with (run / "blocks.jsonl").open("w", encoding="utf-8", newline="\n") as stream:
        for row in all_rows:
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")
    with (run / "blocks.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, CSV_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows({k: str(v).lower() if isinstance(v, bool) else v for k, v in row.items()}
                         for row in all_rows)

    summary = []
    for d in datasets:
        for m in MODES:
            r0, rows = first[(d, m)], rows_by[(d, m)]
            res = r0["result"]
            group_rows = [r for r in raw if r["dataset"] == d and r["mode"] == m]
            summary.append({
                "dataset_id": d, "mode": m, "strategy": LABELS.get(m, m),
                "archive_bytes": res["archive_bytes"],
                "accounting": {"header_bytes": res["header_bytes"], "index_bytes": res["index_bytes"],
                               "data_bytes": res["data_bytes"],
                               "data_bytes_from_blocks": sum(r["stored_bytes"] for r in rows),
                               "seed_and_override_metadata_bytes": None,
                               "seed_and_override_metadata_status":
                                   "not serialized (header strategy_version=0, seed_length=0); missing item",
                               "reconciles": HEADER_BYTES + sum(r["stored_bytes"] for r in rows)
                                             + res["index_bytes"] == res["archive_bytes"]},
                "repetitions": len(group_rows), "deterministic": determinism[f"{d}/{m}"],
                "by_actual_codec": group(rows, lambda r: f"{r['actual_codec']}{r['actual_level'] or ''}"),
                "by_planned_codec": group(rows, lambda r: f"{r['planned_codec']}{r['planned_level'] or ''}"),
                "by_file": group(rows, lambda r: r["file_key"]),
                "by_store_cause": group(rows, lambda r: r["store_cause"] or "compressed"),
                "by_plan_source": group(rows, lambda r: r["plan_source"]),
                **{f"median_{op}_seconds": statistics.median(r[f"{op}_seconds"] for r in group_rows)
                   for op in ("compress", "verify", "extract")},
                "max_compress_peak_working_set_bytes":
                    max(r["compress_peak_working_set_bytes"] for r in group_rows)})
    assert all(s["accounting"]["reconciles"] for s in summary)

    index_bytes = {d: {first[(d, m)]["result"]["index_bytes"] for m in MODES} for d in datasets}
    attrib = {}
    for d in datasets:
        size = {m: first[(d, m)]["result"]["archive_bytes"] for m in MODES}
        attrib[d] = {"index_bytes_identical_across_modes": len(index_bytes[d]) == 1,
                     "archive_bytes": {LABELS.get(m, m): size[m] for m in MODES},
                     "b0_minus_b1_bytes": size["b-global"] - size["zstd3"],
                     "b0_vs_b1": attribution(rows_by, d, "b-global", "zstd3"),
                     "b0_vs_b2_file_codec": attribution(rows_by, d, "b-global", "b2-file-codec")}
        for key in ("b0_vs_b1", "b0_vs_b2_file_codec"):
            ref = "zstd3" if key == "b0_vs_b1" else "b2-file-codec"
            assert sum(c["delta_bytes"] for c in attrib[d][key].values()) == size["b-global"] - size[ref]
    # Tool/schema changes must not alter archives: compare with the B2-r3 run's archive hashes.
    r3_raw = json.loads((R3_RUN / "raw.json").read_text(encoding="utf-8"))
    r3_hash = {(r["dataset"], r["mode"]): r["archive_sha256"] for r in r3_raw}
    r3_equal = {f"{d}/{m}": first[(d, m)]["archive_sha256"] == r3_hash[(d, m)]
                for d in datasets for m in DIRECT_MODES}
    assert all(r3_equal.values()), "archives differ from B2-r3"
    save(run / "summary.json", {"boundaries_identical": boundaries,
                                "archives_identical_to_b2_r3": r3_equal, "rows": summary})
    save(run / "attribution.json", attrib)
    preview(run, raw, summary, attrib, rows_by, datasets)


def preview(run, raw, summary, attrib, rows_by, datasets):
    by = {(s["dataset_id"], s["mode"]): s for s in summary}
    L = ["# B2-r4 P0 逐块诊断与简单基线（由 b2_diagnose.py 从 raw.json 生成）", "",
         f"共 {len(raw)} 次 compress → verify → extract，全部逐字节还原；同组归档 SHA256 一致。",
         "归档字节 = 头部 96 + 索引 + 数据；Seed / 覆盖元数据未序列化（缺项，非 0）。",
         "全部方案在同一数据集上块边界完全相同（file_key、file_block_index、block_id、raw_bytes 逐块一致）。", "",
         "## 完整归档字节（越小越好）", "",
         "| 数据 | " + " | ".join(LABELS.get(m, m) for m in MODES) + " |",
         "| --- |" + " ---: |" * len(MODES)]
    for d in datasets:
        L.append(f"| {d} | " + " | ".join(str(by[(d, m)]["archive_bytes"]) for m in MODES) + " |")
    L += ["", "## 计量对账（B0 / B1 / B2-file-codec）", "",
          "| 数据 | 方案 | 头部 | 索引 | 数据（逐块和） | 合计 | 对账 |", "| --- | --- | ---: | ---: | ---: | ---: | --- |"]
    for d in datasets:
        for m in ("b-global", "zstd3", "b2-file-codec"):
            a = by[(d, m)]["accounting"]
            L.append(f"| {d} | {LABELS[m]} | {a['header_bytes']} | {a['index_bytes']} | "
                     f"{a['data_bytes_from_blocks']} | {by[(d, m)]['archive_bytes']} | "
                     f"{'一致' if a['reconciles'] else '不一致'} |")
    L += ["", "## B0-current 相对 B1-fixed-zstd3 的逐块损失来源", "",
          "| 数据 | 类别 | 块数 | 字节差（B0 − B1） | block_index |", "| --- | --- | ---: | ---: | --- |"]
    for d in datasets:
        for c, v in sorted(attrib[d]["b0_vs_b1"].items(), key=lambda kv: -kv[1]["delta_bytes"]):
            L.append(f"| {d} | {c} | {v['blocks']} | {v['delta_bytes']:+d} | "
                     f"{','.join(map(str, v['block_indexes']))} |")
        L.append(f"| {d} | **合计** | | **{attrib[d]['b0_minus_b1_bytes']:+d}** | |")
    for d in ("text", "mixed"):
        L += ["", f"## {d}：逐块对照（B0 / B1 / B2-file-codec）", "",
              "| block_index | file_key | raw | B0 计划→实际 | B0 stored | B1 实际 | B1 stored | B2 实际 | B2 stored |",
              "| ---: | --- | ---: | --- | ---: | --- | ---: | --- | ---: |"]
        for x, y, z in zip(rows_by[(d, "b-global")], rows_by[(d, "zstd3")], rows_by[(d, "b2-file-codec")]):
            fmt = lambda r: f"{r['actual_codec']}{r['actual_level'] or ''}"
            L.append(f"| {x['block_index']} | {x['file_key']} | {x['raw_bytes']} | "
                     f"{x['planned_codec']}{x['planned_level'] or ''}→{fmt(x)} | {x['stored_bytes']} | "
                     f"{fmt(y)} | {y['stored_bytes']} | {fmt(z)} | {z['stored_bytes']} |")
    L += ["", "## 回退原因汇总（全部方案）", "", "| 数据 | 方案 | 计划 Store | 回退（无空间收益） | 压缩写入 |",
          "| --- | --- | ---: | ---: | ---: |"]
    for d in datasets:
        for m in MODES:
            g = by[(d, m)]["by_store_cause"]
            L.append(f"| {d} | {LABELS.get(m, m)} | {g.get('planned_store', {}).get('blocks', 0)} | "
                     f"{g.get('fallback_no_space_gain', {}).get('blocks', 0)} | "
                     f"{g.get('compressed', {}).get('blocks', 0)} |")
    (run / "preview.md").write_text("\n".join(L) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- execution

def run_mode(tool, run, local, label, mode, inp, target_arg, row):
    archive, out = local / f"{label}.sarc", local / f"{label}-out"
    tool_mode = "file-plan" if mode in PLAN_MODES else mode
    commands = [[str(tool), "compress", tool_mode, str(inp), str(archive), target_arg],
                [str(tool), "verify", str(archive)], [str(tool), "extract", str(archive), str(out)]]
    row["commands"] = commands
    for op, command in zip(("compress", "verify", "extract"), commands):
        result, elapsed = invoke(command, run / "logs" / f"{label}-{op}.log")
        row[f"{op}_seconds"] = elapsed
        row[f"{op}_peak_working_set_bytes"] = result["peak_working_set_bytes"]
        if op == "compress":
            row["result"] = result
    row["byte_exact"] = same_files(inp, out)
    assert row["byte_exact"], "round-trip mismatch"
    row["archive_bytes"] = archive.stat().st_size
    assert row["archive_bytes"] == row["result"]["archive_bytes"]
    row["archive_sha256"] = digest(archive)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--analyze-only", type=Path)
    args = parser.parse_args()
    if args.analyze_only:
        analyze(args.analyze_only.resolve(strict=True))
        return
    if (not args.tool or not args.run_id or not 1 <= args.repetitions <= 10 or len(args.run_id) > 100
            or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.run_id)):
        parser.error("need --tool, a valid --run-id and 1..10 repetitions")
    run, local = ROOT / "research/experiments" / args.run_id, ROOT / "research/local" / args.run_id
    if local.exists() or (run.exists() and any(p.name != "record.md" for p in run.iterdir())):
        parser.error("run exists; use a new run ID")
    source_tool = args.tool.resolve(strict=True)
    (run / "logs").mkdir(parents=True, exist_ok=True)
    (run / "plans").mkdir()
    local.mkdir(parents=True)
    tool = local / source_tool.name
    shutil.copyfile(source_tool, tool)
    shutil.copyfile(__file__, run / "b2_diagnose.py")
    git = lambda *a: subprocess.check_output(["git", "-C", str(ROOT), *a], text=True).strip()
    config = {"status": "running", "run_id": args.run_id, "repetitions": args.repetitions,
              "modes": MODES, "labels": LABELS, "chunk_bytes": 65536,
              "b_rules": {"candidate": {"id": 2, "revision": 1}, "catalog_revision": 1,
                          "overlay_revision": 1, "codec_seed": 0, "lzma2_preset": 6,
                          "zstd_levels": {"b-base": 3, "b-global": 4, "b-file": "6 on target over 4",
                                          "b-cleared": 4}},
              "b2_file_codec_rule": "per file: zstd3 vs lzma6 by total stored bytes from repetition-0 "
                                    "same-boundary fixed runs (tie -> zstd3); then zstd 3->6 file override "
                                    "only if smaller. Store implicit via per-block fallback. Encode-side "
                                    "oracle, 3 trial configs per file; not seed-coded; index already "
                                    "carries per-block codec/config so decode needs no extra metadata.",
              "threads": 1, "seed_search": None, "archive_strategy_version": 0, "archive_seed_length": 0,
              "build": "caller-supplied MSVC Release (msvc-vs2026-release)",
              "timing": "process wall time incl startup, planning, round-trip self-check and JSON evidence",
              "memory": "GetProcessMemoryInfo PeakWorkingSetSize per process; includes tool/report",
              "cache": "no warmup; uncontrolled OS caches; rotate direct-mode order each repetition",
              "tool_sha256": digest(tool), "script_sha256": digest(Path(__file__)),
              "started_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
              "git_commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
              "os": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
              "python": sys.version, "invocation": [sys.executable, *sys.argv],
              "codec_versions": subprocess.check_output([str(tool), "--version"], text=True).strip()}
    save(run / "config.json", config)
    datasets, r3 = fixtures()
    manifest = {"id": "b2-on-disk-r3 (reused)", "r3_manifest_sha256": digest(R3_RUN / "datasets.json"),
                "source_files": r3["source_files"], "datasets": {}}
    for name, (files, target) in datasets.items():
        directory = local / "inputs" / name
        for relative, data in files.items():
            p = directory / relative
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        snap = snapshot(directory)
        manifest["datasets"][name] = {"files": snap, "target": target,
                                      "identical_to_r3": snap == r3["datasets"][name]["files"]}
        assert manifest["datasets"][name]["identical_to_r3"], f"{name} differs from B2-r3 input"
    save(run / "datasets.json", manifest)
    config["dataset_manifest_sha256"] = digest(run / "datasets.json")
    save(run / "config.json", config)
    raw = []
    try:
        trials = {}
        for repeat in range(args.repetitions):
            order = DIRECT_MODES[repeat:] + DIRECT_MODES[:repeat]
            for dataset, (_, target) in datasets.items():
                inp = local / "inputs" / dataset
                results = {}
                for mode in order + PLAN_MODES:
                    label = f"{repeat}-{dataset}-{mode}"
                    if mode in PLAN_MODES:
                        plan_path = run / "plans" / f"{dataset}-{mode}.txt"
                        if repeat == 0:
                            ref = {m: next(r for r in raw if r["dataset"] == dataset and r["mode"] == m
                                           and r["repeat"] == 0)["result"] for m in ("zstd3", "zstd6", "lzma6")}
                            plans, trials[dataset] = file_codec_plans(ref)
                            write_plan(plan_path, plans[mode])
                        target_arg = str(plan_path)
                    else:
                        target_arg = target if mode == "b-file" else "-"
                    row = {"repeat": repeat, "dataset": dataset, "mode": mode, "status": "running"}
                    raw.append(row)
                    try:
                        run_mode(tool, run, local, label, mode, inp, target_arg, row)
                        validate(row["result"], mode, target)
                        row["status"] = "passed"
                    except Exception as error:
                        row["status"], row["error"] = "failed", str(error)
                        raise
                    finally:
                        save(run / "raw.json", raw)
                    results[mode] = row["result"]
                    print(label, row["archive_bytes"], "byte-exact", flush=True)
        save(run / "file-codec-trials.json", trials)
        analyze(run)
        config["status"] = "completed"
    except Exception as error:
        config["status"], config["error"] = "failed", str(error)
        raise
    finally:
        config["finished_at"] = datetime.now(ZoneInfo("America/Los_Angeles")).isoformat()
        save(run / "config.json", config)


if __name__ == "__main__":
    main()
