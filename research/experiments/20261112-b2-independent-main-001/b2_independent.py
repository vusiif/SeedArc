# SPDX-License-Identifier: GPL-3.0-or-later
"""B2-r5 P1: independent evaluation of the frozen adaptive-file-r1 on the held-out test split.

Corpus: research/datasets/<corpus>.json (dev / test fixed by path hash before any compression).
Only the test split is evaluated; dev is reserved for future tuning and is not read here.
Modes: Fixed Zstd-3, Fixed LZMA2-6, Adaptive-file-r1 at its three registered budgets,
Oracle-file (reference upper bound) and, for the seed ablation, b-global (B-r1 seed 0, no search).
Each repetition rotates the mode order; every dataset is read once before measurement (warm OS
cache, documented). Every run is compress -> verify -> extract with byte-exact comparison.

  python research/scripts/b2_independent.py --tool TOOL --run-id RUN [--repetitions 5]
  python research/scripts/b2_independent.py --analyze-only research/experiments/RUN
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
CORPUS = "b2r5-corpus-r1"
FROZEN = ROOT / "research/configs/adaptive-file-r1.frozen.json"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f"{name}.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


r4 = _load("b2_diagnose")
r5 = _load("b2_adaptive")
MODES = ["zstd3", "lzma6", "adaptive-256k", "adaptive-1m", "adaptive-unbounded", "oracle-file", "b-global"]
LABEL = {"zstd3": "Fixed Zstd-3", "lzma6": "Fixed LZMA2-6", "adaptive-256k": "Adaptive-r1 @256 KiB",
         "adaptive-1m": "Adaptive-r1 @1 MiB", "adaptive-unbounded": "Adaptive-r1 @unbounded",
         "oracle-file": "Oracle-file (reference)", "b-global": "B-r1 seed0 (seed, no search)"}
SELECTOR = {"adaptive-256k", "adaptive-1m", "adaptive-unbounded", "oracle-file"}


def stats(values):
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    q = lambda p: v[min(len(v) - 1, max(0, round(p * (len(v) - 1))))]
    return {"n": len(v), "mean": statistics.fmean(v), "median": statistics.median(v), "p10": q(0.10),
            "p90": q(0.90), "min": v[0], "max": v[-1]}


def per_file(result):
    out = {f["path"]: 0 for f in result["files"]}
    for b in result["blocks"]:
        out[b["file_key"]] += b["stored_bytes"]
    return out


def compact(result):
    """raw.json keeps no block list or trial list (the full repetition-0 output is in local/)."""
    small = {k: v for k, v in result.items() if k not in ("blocks", "files")}
    small["file_count"] = len(result["files"])
    small["fallback_blocks"] = sum(1 for b in result["blocks"] if b["planned_codec"] != 0 and b["actual_codec"] == 0)
    if result.get("selector"):
        sel = dict(result["selector"])
        sel["files"] = [{k: v for k, v in f.items() if k != "trials"} for f in sel["files"]]
        small["selector"] = sel
    return small


def load_per_file(run):
    out = {}
    for line in (run / "per_file.csv").read_text(encoding="utf-8").splitlines()[1:]:
        d, m, size, key = line.split(",", 3)
        out.setdefault((d, m), {})[key] = int(size)
    return out


def analyze(run):
    raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
    manifest = json.loads((ROOT / "research/datasets" / f"{CORPUS}.json").read_text(encoding="utf-8"))
    datasets = sorted({r["dataset"] for r in raw})
    rows, files_out = {}, load_per_file(run)
    for d in datasets:
        pool = d.split("/", 1)[1]
        meta = manifest["datasets"][pool]
        for m in MODES:
            g = [r for r in raw if r["dataset"] == d and r["mode"] == m]
            assert g and all(r["status"] == "passed" and r["byte_exact"] for r in g)
            assert len({r["archive_sha256"] for r in g}) == 1, f"nondeterministic {d}/{m}"
            res = g[0]["result"]
            sel = res["selector"]
            dec_w = [r["result"]["selector"]["decision_wall_ms"] if sel else 0.0 for r in g]
            dec_c = [r["result"]["selector"]["decision_cpu_ms"] if sel else 0.0 for r in g]
            rows[(d, m)] = {
                "dataset_id": d, "type": meta["type"], "mode": m, "label": LABEL[m],
                "input_bytes": res["input_bytes"], "files": res["file_count"], "blocks": res["block_count"],
                "fallback_blocks": res["fallback_blocks"],
                "archive_bytes": res["archive_bytes"], "archive_sha256": g[0]["archive_sha256"],
                "ratio": res["archive_bytes"] / max(1, res["input_bytes"]),
                "repetitions": len(g), "byte_exact": True, "deterministic": True,
                "metadata_bytes": None if m == "b-global" else 0,
                "candidates": sel["trial_count"] if sel else 0,
                "trial_input_bytes": sel["budget_used"] if sel else 0,
                "budget_limit": sel["budget_limit"] if sel else None,
                "budget_exhausted_files": sel["budget_exhausted_files"] if sel else 0,
                "decision_reasons": ({k: sum(1 for f in sel["files"] if f["decision_reason"] == k)
                                      for k in sorted({f["decision_reason"] for f in sel["files"]})} if sel else None),
                "compress_wall_ms": stats([r["result"]["call_wall_ms"] for r in g]),
                "compress_cpu_ms": stats([r["result"]["call_cpu_ms"] for r in g]),
                "decision_wall_ms": stats(dec_w), "decision_cpu_ms": stats(dec_c),
                "encode_wall_ms": stats([r["result"]["call_wall_ms"] - x for r, x in zip(g, dec_w)]),
                "extract_wall_ms": stats([r["extract_result"]["call_wall_ms"] for r in g]),
                "extract_cpu_ms": stats([r["extract_result"]["call_cpu_ms"] for r in g]),
                "compress_peak_ws_bytes": max(r["compress_peak_working_set_bytes"] for r in g),
                "extract_peak_ws_bytes": max(r["extract_peak_working_set_bytes"] for r in g)}
    # Per-file savings against Fixed Zstd-3 (same files, same 64 KiB blocks).
    distributions = {}
    for d in datasets:
        base = files_out[(d, "zstd3")]
        for m in MODES:
            if m == "zstd3":
                continue
            other = files_out[(d, m)]
            assert other.keys() == base.keys()
            savings = {k: base[k] - other[k] for k in base}
            distributions[f"{d}/{m}"] = {
                "file_saving_bytes": stats(list(savings.values())),
                "positive": sum(1 for v in savings.values() if v > 0),
                "zero": sum(1 for v in savings.values() if v == 0),
                "negative": sum(1 for v in savings.values() if v < 0),
                "negative_files": sorted(([k, v] for k, v in savings.items() if v < 0), key=lambda x: x[1])[:20]}
    # Cost per KiB saved (median CPU and wall, vs Fixed Zstd-3), per data type.
    cost = {}
    for d in datasets:
        z = rows[(d, "zstd3")]
        for m in MODES:
            if m == "zstd3":
                continue
            r = rows[(d, m)]
            saved = z["archive_bytes"] - r["archive_bytes"]
            dcpu = r["compress_cpu_ms"]["median"] - z["compress_cpu_ms"]["median"]
            dwall = r["compress_wall_ms"]["median"] - z["compress_wall_ms"]["median"]
            cost[f"{d}/{m}"] = {"type": r["type"], "saved_bytes": saved, "extra_cpu_ms": dcpu, "extra_wall_ms": dwall,
                                "cpu_ms_per_kib_saved": dcpu / (saved / 1024) if saved > 0 else None,
                                "wall_ms_per_kib_saved": dwall / (saved / 1024) if saved > 0 else None,
                                "verdict": "saves" if saved > 0 else ("no saving" if saved == 0 else "larger")}
    totals = {m: {"archive_bytes": sum(rows[(d, m)]["archive_bytes"] for d in datasets),
                  "input_bytes": sum(rows[(d, m)]["input_bytes"] for d in datasets),
                  "median_compress_wall_ms": sum(rows[(d, m)]["compress_wall_ms"]["median"] for d in datasets),
                  "median_compress_cpu_ms": sum(rows[(d, m)]["compress_cpu_ms"]["median"] for d in datasets),
                  "median_decision_wall_ms": sum(rows[(d, m)]["decision_wall_ms"]["median"] for d in datasets),
                  "median_extract_wall_ms": sum(rows[(d, m)]["extract_wall_ms"]["median"] for d in datasets),
                  "candidates": sum(rows[(d, m)]["candidates"] for d in datasets),
                  "trial_input_bytes": sum(rows[(d, m)]["trial_input_bytes"] for d in datasets),
                  "budget_exhausted_files": sum(rows[(d, m)]["budget_exhausted_files"] for d in datasets)}
              for m in MODES}
    relative = {m: stats([rows[(d, m)]["archive_bytes"] / rows[(d, "zstd3")]["archive_bytes"] - 1 for d in datasets])
                for m in MODES}
    r4.save(run / "summary.json", {"rows": [rows[(d, m)] for d in datasets for m in MODES],
                                   "per_file_vs_zstd3": distributions, "cost_vs_zstd3": cost, "totals": totals,
                                   "dataset_relative_size_vs_zstd3": relative})
    preview(run, raw, rows, datasets, distributions, cost, totals, relative)


def preview(run, raw, rows, datasets, dist, cost, totals, relative):
    f = lambda v, n=1: "—" if v is None else f"{v:,.{n}f}"
    L = ["# B2-r5 P1 independent evaluation (generated by b2_independent.py from raw.json)", "",
         f"{len(raw)} compress → verify → extract runs on the held-out test split, all byte-exact; "
         "every dataset/mode produced the same archive SHA256 in all repetitions.",
         "Times are medians over repetitions of the in-tool compress / extract call (decision included); "
         "CPU = process user+kernel (≈15.6 ms resolution). Warm OS cache (each dataset read once before measuring).", "",
         "## Complete archive bytes (test split)", "",
         "| dataset | type | input B | " + " | ".join(LABEL[m] for m in MODES) + " |",
         "| --- | --- | ---: |" + " ---: |" * len(MODES)]
    for d in datasets:
        z = rows[(d, "zstd3")]
        L.append(f"| {d} | {z['type']} | {z['input_bytes']:,} | " +
                 " | ".join(f"{rows[(d, m)]['archive_bytes']:,}" for m in MODES) + " |")
    L.append("| **total** | | " + f"{totals['zstd3']['input_bytes']:,} | " +
             " | ".join(f"**{totals[m]['archive_bytes']:,}**" for m in MODES) + " |")
    L += ["", "## Relative size vs Fixed Zstd-3 across the 10 datasets", "",
          "| mode | mean | median | p10 | p90 | min | max |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for m in MODES:
        s = relative[m]
        L.append(f"| {LABEL[m]} | " + " | ".join(f"{s[k] * 100:+.3f}%" for k in ("mean", "median", "p10", "p90", "min", "max")) + " |")
    L += ["", "## Cost (median ms) and budget use", "",
          "| dataset | mode | compress wall | compress CPU | decision wall | encode wall | extract wall | extract CPU | candidates | trial input B | exhausted | peak WS MiB (c/x) |",
          "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for d in datasets:
        for m in MODES:
            r = rows[(d, m)]
            L.append(f"| {d} | {LABEL[m]} | {f(r['compress_wall_ms']['median'])} | {f(r['compress_cpu_ms']['median'])} | "
                     f"{f(r['decision_wall_ms']['median'])} | {f(r['encode_wall_ms']['median'])} | {f(r['extract_wall_ms']['median'])} | "
                     f"{f(r['extract_cpu_ms']['median'])} | {r['candidates']} | {r['trial_input_bytes']:,} | {r['budget_exhausted_files']} | "
                     f"{r['compress_peak_ws_bytes'] / 2**20:.1f}/{r['extract_peak_ws_bytes'] / 2**20:.1f} |")
    L += ["", "## Extra compress cost per KiB saved vs Fixed Zstd-3 (by data type)", "",
          "| dataset | type | mode | saved B | extra CPU ms | CPU ms / KiB saved | extra wall ms | wall ms / KiB saved | verdict |",
          "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |"]
    for d in datasets:
        for m in MODES[1:]:
            c = cost[f"{d}/{m}"]
            L.append(f"| {d} | {c['type']} | {LABEL[m]} | {c['saved_bytes']:+,} | {f(c['extra_cpu_ms'])} | "
                     f"{f(c['cpu_ms_per_kib_saved'], 2)} | {f(c['extra_wall_ms'])} | {f(c['wall_ms_per_kib_saved'], 2)} | {c['verdict']} |")
    L += ["", "## Per-file saving vs Fixed Zstd-3 (bytes; same files and blocks)", "",
          "| dataset | mode | files +/0/− | mean | median | p10 | p90 | worst |", "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for d in datasets:
        for m in MODES[1:]:
            x = dist[f"{d}/{m}"]
            s = x["file_saving_bytes"]
            L.append(f"| {d} | {LABEL[m]} | {x['positive']}/{x['zero']}/{x['negative']} | {f(s['mean'])} | {f(s['median'], 0)} | "
                     f"{f(s['p10'], 0)} | {f(s['p90'], 0)} | {f(s['min'], 0)} |")
    L += ["", "## Negative and no-gain samples", ""]
    neg = [(k, c) for k, c in cost.items() if c["verdict"] != "saves"]
    L += [f"- {k}: {c['verdict']} ({c['saved_bytes']:+,} B, extra CPU {f(c['extra_cpu_ms'])} ms)" for k, c in neg] or ["- none"]
    L += ["", "Files where a mode is larger than Fixed Zstd-3 (up to 20 per dataset/mode):", ""]
    any_neg = False
    for k, x in dist.items():
        if x["negative"]:
            any_neg = True
            L.append(f"- {k}: {x['negative']} files, e.g. " + ", ".join(f"{n} ({v:+,} B)" for n, v in x["negative_files"][:5]))
    if not any_neg:
        L.append("- none")
    L += ["", "## Totals", "", "| mode | archive B | vs Zstd-3 | compress wall ms | compress CPU ms | decision wall ms | extract wall ms | candidates | trial input B | exhausted files |",
          "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for m in MODES:
        t = totals[m]
        L.append(f"| {LABEL[m]} | {t['archive_bytes']:,} | {t['archive_bytes'] - totals['zstd3']['archive_bytes']:+,} | "
                 f"{t['median_compress_wall_ms']:,.0f} | {t['median_compress_cpu_ms']:,.0f} | {t['median_decision_wall_ms']:,.0f} | "
                 f"{t['median_extract_wall_ms']:,.0f} | {t['candidates']} | {t['trial_input_bytes']:,} | {t['budget_exhausted_files']} |")
    (run / "preview.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--analyze-only", type=Path)
    args = parser.parse_args()
    if args.analyze_only:
        analyze(args.analyze_only.resolve(strict=True))
        return
    if (not args.tool or not args.run_id or not 1 <= args.repetitions <= 20 or len(args.run_id) > 100
            or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.run_id)):
        parser.error("need --tool, a valid --run-id and 1..20 repetitions")
    run, local = ROOT / "research/experiments" / args.run_id, ROOT / "research/local" / args.run_id
    if local.exists() or (run.exists() and any(p.name != "record.md" for p in run.iterdir())):
        parser.error("run exists; use a new run ID")
    corpus_root = ROOT / "research/local" / CORPUS / "test"
    manifest_path = ROOT / "research/datasets" / f"{CORPUS}.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    source_tool = args.tool.resolve(strict=True)
    (run / "logs").mkdir(parents=True, exist_ok=True)
    local.mkdir(parents=True)
    tool = local / source_tool.name
    shutil.copyfile(source_tool, tool)
    for s in ("b2_independent.py", "b2_adaptive.py", "b2_diagnose.py"):
        shutil.copyfile(Path(__file__).with_name(s), run / s)
    shutil.copyfile(FROZEN, run / FROZEN.name)
    # Verify the test split against its listings before measuring (independent-set integrity).
    datasets = []
    for pool, entry in sorted(manifest["datasets"].items()):
        split = entry["splits"]["test"]
        listing = (ROOT / "research/datasets" / split["listing"]).read_text(encoding="utf-8")
        expected = {line.split("\t")[0]: (int(line.split("\t")[1]), line.split("\t")[2]) for line in listing.splitlines()}
        snap = r4.snapshot(corpus_root / pool)
        assert {k: (v["bytes"], v["sha256"]) for k, v in snap.items()} == expected, f"test/{pool} differs from manifest"
        datasets.append(pool)
    git = lambda *a: subprocess.check_output(["git", "-C", str(ROOT), *a], text=True).strip()
    config = {"status": "running", "run_id": args.run_id, "corpus": CORPUS, "split": "test",
              "manifest_sha256": r4.digest(manifest_path), "frozen_config_sha256": r4.digest(FROZEN),
              "repetitions": args.repetitions, "modes": MODES, "labels": LABEL, "chunk_bytes": 65536, "threads": 1,
              "order": "mode order rotated by one position per repetition; datasets in sorted order",
              "cache": "warm: every dataset fully read (hashed) once before its first measured run; no reboot / flush",
              "timing": "call_wall/cpu around compress and extract inside the tool; decision measured inside the planner",
              "memory": "GetProcessMemoryInfo PeakWorkingSetSize per process (includes tool and JSON report)",
              "tool_sha256": r4.digest(tool), "script_sha256": r4.digest(Path(__file__)),
              "started_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
              "git_commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
              "os": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
              "python": sys.version, "invocation": [sys.executable, *sys.argv],
              "codec_versions": subprocess.check_output([str(tool), "--version"], text=True).strip()}
    r4.save(run / "config.json", config)
    raw = []
    (run / "per_file.csv").write_text("dataset,mode,stored_bytes,file_key\n", encoding="utf-8", newline="\n")
    try:
        for pool in datasets:
            r4.snapshot(corpus_root / pool)  # warm read
        for repeat in range(args.repetitions):
            order = MODES[repeat % len(MODES):] + MODES[:repeat % len(MODES)]
            for pool in datasets:
                inp = corpus_root / pool
                for mode in order:
                    label = f"{repeat}-{pool}-{mode}"
                    tmode, targ = r5.tool_args(mode, "-")
                    archive, out = local / f"{label}.sarc", local / f"{label}-out"
                    commands = [[str(tool), "compress", tmode, str(inp), str(archive), targ],
                                [str(tool), "verify", str(archive)], [str(tool), "extract", str(archive), str(out)]]
                    row = {"repeat": repeat, "dataset": f"test/{pool}", "mode": mode, "status": "running"}
                    raw.append(row)
                    try:
                        for op, command in zip(("compress", "verify", "extract"), commands):
                            result, elapsed = invoke(command, run / "logs" / f"{label}-{op}.log")
                            row[f"{op}_seconds"] = elapsed
                            row[f"{op}_peak_working_set_bytes"] = result["peak_working_set_bytes"]
                            row["result" if op == "compress" else f"{op}_result"] = result
                        r5.validate(row["result"], mode)
                        if repeat == 0:
                            (local / "rep0").mkdir(exist_ok=True)
                            (local / "rep0" / f"{pool}-{mode}.json").write_text(
                                json.dumps(row["result"], ensure_ascii=False), encoding="utf-8")
                            with (run / "per_file.csv").open("a", encoding="utf-8", newline="\n") as stream:
                                for key, size in sorted(per_file(row["result"]).items()):
                                    stream.write(f"test/{pool},{mode},{size},{key}\n")
                        row["result"] = compact(row["result"])
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
                    archive.unlink()
                    shutil.rmtree(out)
                    print(label, row["archive_bytes"], "byte-exact", flush=True)
        analyze(run)
        config["status"] = "completed"
    except Exception as error:
        config["status"], config["error"] = "failed", str(error)
        raise
    finally:
        config["finished_at"] = datetime.now(ZoneInfo("America/Los_Angeles")).isoformat()
        r4.save(run / "config.json", config)


def invoke(command, log):
    """Like b2_diagnose.invoke but with a longer timeout and the log trimmed (block lists are large)."""
    import time
    start = time.perf_counter()
    result = subprocess.run(command, capture_output=True, timeout=1800)
    elapsed = time.perf_counter() - start
    log.write_bytes(result.stdout[:4096] + b"\n...\n--- stderr ---\n" + result.stderr)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {log}")
    data = json.loads(result.stdout)
    if not isinstance(data.get("peak_working_set_bytes"), int) or data["peak_working_set_bytes"] <= 0:
        raise RuntimeError("peak working set measurement unavailable")
    return data, elapsed


if __name__ == "__main__":
    main()
