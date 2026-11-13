# SPDX-License-Identifier: GPL-3.0-or-later
"""B2-r5 P0: reproducible baseline, Adaptive-file selector and unified-budget comparison.

Reuses the B2-r3/r4 on-disk fixtures (hash-checked) with the same 64 KiB blocks. Every mode runs
compress -> verify -> extract with byte-exact comparison. P0-A: the eight r4 direct modes and
Oracle-file must reproduce the r4 archive SHA256 (Oracle against r4's B2-file-codec). P0-C: Fixed
Zstd-3, Fixed LZMA2-6, Adaptive-file at registered budgets and Oracle-file, all costs included.
Stdlib only. Parameters are registered in research/ideas/seed-adaptive-file-r5.md; do not tune.

  python research/scripts/b2_adaptive.py --tool TOOL --run-id RUN [--repetitions 3]
  python research/scripts/b2_adaptive.py --analyze-only research/experiments/RUN
"""
import argparse
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
R4_RUN = ROOT / "research/experiments/20261112-b2-diagnose-main-001"
_spec = importlib.util.spec_from_file_location("b2_diagnose", Path(__file__).with_name("b2_diagnose.py"))
r4 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(r4)

SCHEMA = "seedarc.b2-compression.r5"
REGRESSION = ["store", "zstd3", "zstd6", "lzma6", "b-base", "b-global", "b-file", "b-cleared"]
BUDGETS = {"adaptive-0": "0", "adaptive-256k": "262144", "adaptive-1m": "1048576",
           "adaptive-unbounded": "unbounded"}
SELECTOR_MODES = ["oracle-file", *BUDGETS]
MODES = REGRESSION + SELECTOR_MODES
MAIN = ["zstd3", "lzma6", "adaptive-0", "adaptive-256k", "adaptive-1m", "adaptive-unbounded", "oracle-file"]
LABEL = {"zstd3": "Fixed Zstd-3", "lzma6": "Fixed LZMA2-6", "oracle-file": "Oracle-file（参考上限）",
         **{m: f"Adaptive-file @ {b}" for m, b in BUDGETS.items()}}
R4_EQUIV = {**{m: m for m in REGRESSION}, "oracle-file": "b2-file-codec"}


def tool_args(mode, target):
    if mode in BUDGETS:
        return "adaptive-file", BUDGETS[mode]
    if mode == "oracle-file":
        return "oracle-file", "-"
    return mode, target if mode == "b-file" else "-"


def validate(result, mode):
    assert result["schema"] == SCHEMA
    assert result["header_bytes"] + result["data_bytes"] + result["index_bytes"] == result["archive_bytes"]
    assert sum(b["stored_bytes"] for b in result["blocks"]) == result["data_bytes"]
    assert result["strategy_version"] == 0 and result["seed_length"] == 0
    sel = result["selector"]
    if mode not in SELECTOR_MODES:
        assert sel is None
        return
    assert sel["selector_version"] == ("oracle-file-r1" if mode == "oracle-file" else "adaptive-file-r1")
    limit = None if mode in ("oracle-file", "adaptive-unbounded") else int(BUDGETS[mode])
    assert sel["budget_limit"] == limit
    assert limit is None or sel["budget_used"] <= limit
    trials = [t for f in sel["files"] for t in f["trials"]]
    assert sum(t["trial_input_bytes"] for t in trials) == sel["budget_used"] and len(trials) == sel["trial_count"]
    decided = {f["file_key"]: (f["selected_codec"], f["selected_level"]) for f in sel["files"]}
    name = {0: "store", 1: "zstd", 2: "lzma2"}
    for b in result["blocks"]:
        assert (name[b["planned_codec"]], b["planned_config"]) == decided[b["file_key"]]
        if b["actual_codec"] != 0:
            assert (b["actual_codec"], b["actual_config"]) == (b["planned_codec"], b["planned_config"])


def med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def analyze(run):
    raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
    r4_raw = json.loads((R4_RUN / "raw.json").read_text(encoding="utf-8"))
    r4_hash = {(r["dataset"], r["mode"]): r["archive_sha256"] for r in r4_raw}
    datasets = sorted({r["dataset"] for r in raw})
    rows = {}
    for d in datasets:
        for m in MODES:
            g = [r for r in raw if r["dataset"] == d and r["mode"] == m]
            assert g and all(r["status"] == "passed" and r["byte_exact"] for r in g)
            assert len({r["archive_sha256"] for r in g}) == 1, f"nondeterministic {d}/{m}"
            res = g[0]["result"]
            sel = res["selector"]
            plans = {json.dumps([(b["planned_codec"], b["planned_config"]) for b in r["result"]["blocks"]]) for r in g}
            decisions = {json.dumps([(f["file_key"], f["selected_codec"], f["selected_level"], f["decision_reason"])
                                     for f in r["result"]["selector"]["files"]]) for r in g} if sel else set()
            assert len(plans) == 1 and len(decisions) <= 1, f"decision drift {d}/{m}"
            dec_wall = [r["result"]["selector"]["decision_wall_ms"] if sel else 0.0 for r in g]
            dec_cpu = [r["result"]["selector"]["decision_cpu_ms"] if sel else 0.0 for r in g]
            call_wall = [r["result"]["call_wall_ms"] for r in g]
            call_cpu = [r["result"]["call_cpu_ms"] for r in g]
            rows[(d, m)] = {
                "dataset_id": d, "mode": m, "label": LABEL.get(m, m), "archive_bytes": res["archive_bytes"],
                "archive_sha256": g[0]["archive_sha256"], "repetitions": len(g),
                "byte_exact": all(r["byte_exact"] for r in g), "deterministic": True,
                "metadata_bytes": None if m.startswith("b-") else 0,
                "metadata_note": ("B seed/override not serialized: missing item" if m.startswith("b-") else
                                  "decision fully expressed by existing per-block index codec/config"),
                "r4_reference_mode": R4_EQUIV.get(m),
                "identical_to_r4": (g[0]["archive_sha256"] == r4_hash[(d, R4_EQUIV[m])]) if m in R4_EQUIV else None,
                "candidates": sel["trial_count"] if sel else 0,
                "trial_input_bytes": sel["budget_used"] if sel else 0,
                "selection_read_bytes": sel["read_bytes"] if sel else 0,
                "budget_limit": sel["budget_limit"] if sel else None,
                "budget_exhausted_files": sel["budget_exhausted_files"] if sel else 0,
                "decisions": [{k: f[k] for k in ("file_key", "file_bytes", "selected_codec", "selected_level",
                                                  "decision_reason", "notes", "sampled_blocks")}
                              for f in sel["files"]] if sel else None,
                "median_decision_wall_ms": med(dec_wall), "median_decision_cpu_ms": med(dec_cpu),
                "median_encode_wall_ms": med([c - x for c, x in zip(call_wall, dec_wall)]),
                "median_encode_cpu_ms": med([c - x for c, x in zip(call_cpu, dec_cpu)
                                             if c is not None and x is not None]),
                "median_total_wall_ms": med(call_wall), "median_total_cpu_ms": med(call_cpu),
                "median_compress_process_wall_ms": med([r["compress_seconds"] * 1000 for r in g]),
                "median_extract_call_wall_ms": med([r["extract_result"]["call_wall_ms"] for r in g]),
                "median_extract_call_cpu_ms": med([r["extract_result"]["call_cpu_ms"] for r in g]),
                "median_extract_process_wall_ms": med([r["extract_seconds"] * 1000 for r in g]),
                "max_compress_peak_working_set_bytes": max(r["compress_peak_working_set_bytes"] for r in g),
                "max_extract_peak_working_set_bytes": max(r["extract_peak_working_set_bytes"] for r in g)}
    p0a = {f"{d}/{m}": rows[(d, m)]["identical_to_r4"] for d in datasets for m in R4_EQUIV}
    assert all(p0a.values()), "P0-A reproduction failed"
    totals = {}
    for m in MAIN:
        totals[m] = {"archive_bytes": sum(rows[(d, m)]["archive_bytes"] for d in datasets),
                     **{k: sum(rows[(d, m)][k] or 0 for d in datasets)
                        for k in ("candidates", "trial_input_bytes", "budget_exhausted_files",
                                  "median_decision_wall_ms", "median_encode_wall_ms", "median_total_wall_ms",
                                  "median_decision_cpu_ms", "median_total_cpu_ms", "median_extract_call_wall_ms")}}
    hypotheses = {
        "H1_never_worse_than_zstd3": {f"{d}/{m}": rows[(d, m)]["archive_bytes"] <= rows[(d, "zstd3")]["archive_bytes"]
                                      for d in datasets for m in BUDGETS},
        "H2_unbounded_within_half_of_oracle_saving": {
            d: (rows[(d, "adaptive-unbounded")]["archive_bytes"] - rows[(d, "oracle-file")]["archive_bytes"]) * 2
               <= rows[(d, "zstd3")]["archive_bytes"] - rows[(d, "oracle-file")]["archive_bytes"]
            for d in datasets}}
    r4.save(run / "summary.json", {"p0a_identical_to_r4": p0a, "hypotheses": hypotheses, "totals": totals,
                                   "rows": [rows[(d, m)] for d in datasets for m in MODES]})
    preview(run, raw, rows, totals, datasets, hypotheses)
    svg(run, totals)


def preview(run, raw, rows, totals, datasets, hyp):
    fmt = lambda v, n=2: "—" if v is None else f"{v:.{n}f}"
    L = ["# B2-r5 P0 结果（由 b2_adaptive.py 从 raw.json 生成）", "",
         f"共 {len(raw)} 次 compress → verify → extract，全部逐字节还原，同组归档 SHA256 一致、决策一致。",
         "P0-A：8 个 r4 原方案与 Oracle-file（对 r4 B2-file-codec）共 45 组归档 SHA256 与 B2-r4 相同。" if all(
             r["identical_to_r4"] for r in rows.values() if r["identical_to_r4"] is not None) else "P0-A 不一致！",
         "时间为工具内 compress 调用（不含进程启动与 JSON 输出）的中位数；CPU 为进程/线程 user+kernel，Windows 计时粒度约 15.6 ms，毫秒级 CPU 值只作量级参考。", "",
         "## 完整归档字节", "", "| 数据 | " + " | ".join(LABEL[m] for m in MAIN) + " |",
         "| --- |" + " ---: |" * len(MAIN)]
    for d in datasets:
        L.append(f"| {d} | " + " | ".join(str(rows[(d, m)]["archive_bytes"]) for m in MAIN) + " |")
    L.append("| **合计** | " + " | ".join(f"**{totals[m]['archive_bytes']}**" for m in MAIN) + " |")
    L += ["", "## 成本（每数据集中位数，ms）", "",
          "| 数据 | 方案 | 候选数 | 试编码输入 B | 预算耗尽文件 | 决策墙钟 | 决策 CPU | 最终编码墙钟 | 合计墙钟 | 合计 CPU | 解压墙钟 | 压缩峰值 MiB | 解压峰值 MiB |",
          "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for d in datasets:
        for m in MAIN:
            r = rows[(d, m)]
            L.append(f"| {d} | {LABEL[m]} | {r['candidates']} | {r['trial_input_bytes']} | {r['budget_exhausted_files']} | "
                     f"{fmt(r['median_decision_wall_ms'])} | {fmt(r['median_decision_cpu_ms'], 1)} | "
                     f"{fmt(r['median_encode_wall_ms'])} | {fmt(r['median_total_wall_ms'])} | {fmt(r['median_total_cpu_ms'], 1)} | "
                     f"{fmt(r['median_extract_call_wall_ms'])} | {r['max_compress_peak_working_set_bytes'] / 2**20:.2f} | "
                     f"{r['max_extract_peak_working_set_bytes'] / 2**20:.2f} |")
    L += ["", "## 五组合计（大小—时间）", "", "| 方案 | 归档合计 B | 相对 Zstd-3 | 候选 | 试编码输入 B | 决策墙钟 ms | 合计墙钟 ms |",
          "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    base = totals["zstd3"]["archive_bytes"]
    for m in MAIN:
        t = totals[m]
        L.append(f"| {LABEL[m]} | {t['archive_bytes']} | {t['archive_bytes'] - base:+d} | {t['candidates']} | "
                 f"{t['trial_input_bytes']} | {t['median_decision_wall_ms']:.2f} | {t['median_total_wall_ms']:.2f} |")
    L += ["", "![归档大小与合计墙钟](size-vs-time.svg)", "", "## Adaptive-file 决策", "",
          "| 数据 | 预算档 | file_key | 选择 | decision_reason | notes | 抽样块 |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for d in datasets:
        for m in BUDGETS:
            for f in rows[(d, m)]["decisions"]:
                L.append(f"| {d} | {BUDGETS[m]} | {f['file_key']} | {f['selected_codec']}{f['selected_level']} | "
                         f"{f['decision_reason']} | {','.join(f['notes']) or '—'} | {','.join(map(str, f['sampled_blocks'])) or '—'} |")
    L += ["", "## 预先登记假设", "",
          f"- H1（各档不大于 Zstd-3）：{'成立' if all(hyp['H1_never_worse_than_zstd3'].values()) else '不成立'}",
          f"- H2（无界档与 Oracle 差距 ≤ Oracle 节省量一半）：" + "，".join(
              f"{d} {'成立' if v else '不成立'}" for d, v in hyp["H2_unbounded_within_half_of_oracle_saving"].items())]
    (run / "preview.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def svg(run, totals):
    pts = [(LABEL[m], totals[m]["median_total_wall_ms"], totals[m]["archive_bytes"] - totals["zstd3"]["archive_bytes"])
           for m in MAIN]
    W, H, P = 720, 420, 70
    xs, ys = [p[1] for p in pts], [p[2] for p in pts]
    x0, x1 = 0, max(xs) * 1.1
    y0, y1 = min(ys + [0]) * 1.1 - 50, max(ys + [0]) * 1.1 + 50
    X = lambda v: P + (v - x0) / (x1 - x0) * (W - 2 * P)
    Y = lambda v: H - P - (v - y0) / (y1 - y0) * (H - 2 * P)
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="#fff"/>',
         f'<line x1="{P}" y1="{H-P}" x2="{W-P}" y2="{H-P}" stroke="#333"/>',
         f'<line x1="{P}" y1="{P}" x2="{P}" y2="{H-P}" stroke="#333"/>',
         f'<line x1="{P}" y1="{Y(0):.1f}" x2="{W-P}" y2="{Y(0):.1f}" stroke="#bbb" stroke-dasharray="4 3"/>',
         f'<text x="{W/2}" y="{H-25}" text-anchor="middle">五组合计：compress 调用墙钟中位数之和 (ms，含决策)</text>',
         f'<text x="18" y="{H/2}" transform="rotate(-90 18 {H/2})" text-anchor="middle">归档合计 − Fixed Zstd-3 (B)</text>',
         f'<text x="{P}" y="{P-20}" font-weight="bold">B2-r5：归档大小与时间（3 次中位数，单机，非显著性结论）</text>']
    for v in (x0, x1 / 2, x1):
        s.append(f'<text x="{X(v):.1f}" y="{H-P+16}" text-anchor="middle">{v:.0f}</text>')
    for v in (y0, 0, y1):
        s.append(f'<text x="{P-6}" y="{Y(v)+4:.1f}" text-anchor="end">{v:.0f}</text>')
    for i, (name, x, y) in enumerate(pts):
        s.append(f'<circle cx="{X(x):.1f}" cy="{Y(y):.1f}" r="5" fill="#2a6fb0"/>')
        s.append(f'<text x="{X(x)+8:.1f}" y="{Y(y) - 8 + (i % 3) * 14:.1f}">{name} ({y:+d} B, {x:.0f} ms)</text>')
    s.append("</svg>")
    (run / "size-vs-time.svg").write_text("\n".join(s) + "\n", encoding="utf-8")


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
    local.mkdir(parents=True)
    tool = local / source_tool.name
    shutil.copyfile(source_tool, tool)
    for script in (Path(__file__), Path(__file__).with_name("b2_diagnose.py")):
        shutil.copyfile(script, run / script.name)
    git = lambda *a: subprocess.check_output(["git", "-C", str(ROOT), *a], text=True).strip()
    cache = (ROOT / "build/msvc-vs2026-release/CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    build_flags = {line.split("=", 1)[0]: line.split("=", 1)[1] for line in cache.splitlines()
                   if line.startswith(("CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:", "CMAKE_CXX_COMPILER:",
                                       "CMAKE_GENERATOR:", "VCPKG_TARGET_TRIPLET:", "CMAKE_TOOLCHAIN_FILE:",
                                       "SEEDARC_BUILD_RESEARCH_TOOLS:"))}
    config = {"status": "running", "run_id": args.run_id, "repetitions": args.repetitions, "modes": MODES,
              "labels": LABEL, "budgets": BUDGETS, "chunk_bytes": 65536, "threads": 1,
              "selector": {"version": "adaptive-file-r1", "min_file_bytes": 16384, "min_gain_permille": 20,
                           "min_gain_bytes": 64, "samples": "{0, n/2, n-1}", "trial_order": ["zstd3", "lzma6", "zstd6"],
                           "registered_in": "research/ideas/seed-adaptive-file-r5.md"},
              "oracle": "oracle-file-r1: all blocks x {zstd3, zstd6, lzma6} in-process; r4 B2-file-codec rule",
              "timing": ("call_wall/cpu: inside the tool around compress (includes the selector's decision); "
                         "decision_*: inside the planner; encode = call - decision; process wall from Python "
                         "includes startup/JSON. CPU: GetProcessTimes / GetThreadTimes (≈15.6 ms resolution)"),
              "memory": "GetProcessMemoryInfo PeakWorkingSetSize per process; includes tool/report",
              "cache": "no warmup; uncontrolled OS caches; rotate mode order each repetition",
              "tool_sha256": r4.digest(tool), "script_sha256": r4.digest(Path(__file__)),
              "build": {"preset": "msvc-vs2026-release", "config": "Release", **build_flags},
              "vcpkg_manifest_sha256": r4.digest(ROOT / "vcpkg.json"),
              "started_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
              "git_commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
              "os": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
              "python": sys.version, "invocation": [sys.executable, *sys.argv],
              "codec_versions": subprocess.check_output([str(tool), "--version"], text=True).strip()}
    r4.save(run / "config.json", config)
    datasets, r3 = r4.fixtures()
    manifest = {"id": "b2-on-disk-r3 (reused)", "source_files": r3["source_files"], "datasets": {}}
    for name, (files, target) in datasets.items():
        directory = local / "inputs" / name
        for relative, data in files.items():
            p = directory / relative
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        snap = r4.snapshot(directory)
        assert snap == r3["datasets"][name]["files"], f"{name} differs from B2-r3/r4 input"
        manifest["datasets"][name] = {"files": snap, "target": target, "identical_to_r3_r4": True}
    r4.save(run / "datasets.json", manifest)
    config["dataset_manifest_sha256"] = r4.digest(run / "datasets.json")
    r4.save(run / "config.json", config)
    raw = []
    try:
        for repeat in range(args.repetitions):
            order = MODES[repeat:] + MODES[:repeat]
            for dataset, (_, target) in datasets.items():
                inp = local / "inputs" / dataset
                for mode in order:
                    label = f"{repeat}-{dataset}-{mode}"
                    tmode, targ = tool_args(mode, target)
                    archive, out = local / f"{label}.sarc", local / f"{label}-out"
                    commands = [[str(tool), "compress", tmode, str(inp), str(archive), targ],
                                [str(tool), "verify", str(archive)], [str(tool), "extract", str(archive), str(out)]]
                    row = {"repeat": repeat, "dataset": dataset, "mode": mode, "status": "running", "commands": commands}
                    raw.append(row)
                    try:
                        for op, command in zip(("compress", "verify", "extract"), commands):
                            result, elapsed = r4.invoke(command, run / "logs" / f"{label}-{op}.log")
                            row[f"{op}_seconds"] = elapsed
                            row[f"{op}_peak_working_set_bytes"] = result["peak_working_set_bytes"]
                            row["result" if op == "compress" else f"{op}_result"] = result
                        validate(row["result"], mode)
                        row["byte_exact"] = r4.same_files(inp, out)
                        assert row["byte_exact"], "round-trip mismatch"
                        row["archive_bytes"] = archive.stat().st_size
                        assert row["archive_bytes"] == row["result"]["archive_bytes"]
                        row["archive_sha256"] = r4.digest(archive)
                        row["status"] = "passed"
                    except Exception as error:
                        row["status"], row["error"] = "failed", str(error)
                        raise
                    finally:
                        r4.save(run / "raw.json", raw)
                    print(label, row["archive_bytes"], "byte-exact", flush=True)
        analyze(run)
        config["status"] = "completed"
    except Exception as error:
        config["status"], config["error"] = "failed", str(error)
        raise
    finally:
        config["finished_at"] = datetime.now(ZoneInfo("America/Los_Angeles")).isoformat()
        r4.save(run / "config.json", config)


if __name__ == "__main__":
    main()
