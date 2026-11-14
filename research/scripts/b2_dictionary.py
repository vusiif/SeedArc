# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-013 shared-dictionary feasibility on b2r5-corpus-r2 (stdlib only).

  --phase dev  : parameter sweep on the dev split; writes selection.json by the registered rule
  --phase test : ONE formal run on the held-out test split with the selection from a dev run
  --analyze-only RUN_DIR : regenerate summary / preview from raw.json

Rules, grid and decision criteria: research/ideas/shared-dictionary-r1.md (registered first).
Every run is pack -> verify (in-memory byte compare) -> peek (cold single-file extractions);
production references (zstd3 / lzma6) are compress -> verify -> extract -> byte compare -> peek.
"""
import argparse
from datetime import datetime
import hashlib
import importlib.util
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
CORPUS = "b2r5-corpus-r2"
POOLS = ["many-small-docs", "source-python", "config-cmake-pkg"]
LEVELS = [3, 9]
DICT_SIZES = [16384, 65536, 114688, 262144]
TRAIN = [1 << 20, 4 << 20, 0]  # 0: all input
PEEK = 30


def _load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f"{name}.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


corpus = _load("b2_corpus_r2")
r4 = _load("b2_diagnose")


def label(cfg):
    if cfg["kind"] == "prod":
        return f"P-{cfg['mode']}"
    if cfg["dict"] == 0:
        return f"C-nodict-{cfg['level']}"
    return f"C-dict-{cfg['level']}-D{cfg['dict'] // 1024}k-T{cfg['train'] // (1 << 20) if cfg['train'] else 'all'}"


def configs(phase, selection):
    out = [{"kind": "prod", "mode": m} for m in ("zstd3", "lzma6")]
    for level in LEVELS:
        out.append({"kind": "container", "level": level, "dict": 0, "train": 0})
        if phase == "dev":
            out += [{"kind": "container", "level": level, "dict": d, "train": t} for d in DICT_SIZES for t in TRAIN]
        else:
            s = selection[str(level)]
            out.append({"kind": "container", "level": level, "dict": s["dict"], "train": s["train"]})
    return out


def run_json(cmd, log, timeout=1800):
    start = time.perf_counter()
    p = subprocess.run([str(c) for c in cmd], capture_output=True, timeout=timeout)
    elapsed = (time.perf_counter() - start) * 1000
    log.write_bytes(p.stdout[:4096] + b"\n--- stderr ---\n" + p.stderr)
    if p.returncode:
        raise RuntimeError(f"exit {p.returncode}: {cmd[1:3]} -> {p.stderr.decode(errors='replace')[:300]}")
    return json.loads(p.stdout), elapsed


def one(tools, cfg, inp, work, logs, tag):
    dict_tool, sarc_tool = tools
    row = {"config": label(cfg), **cfg}
    if cfg["kind"] == "prod":
        archive, out = work / f"{tag}.sarc", work / f"{tag}-out"
        c, row["compress_process_ms"] = run_json([sarc_tool, "compress", cfg["mode"], inp, archive, "-"], logs / f"{tag}-c.log")
        v, _ = run_json([sarc_tool, "verify", archive], logs / f"{tag}-v.log")
        x, _ = run_json([sarc_tool, "extract", archive, out], logs / f"{tag}-x.log")
        row["byte_exact"] = r4.same_files(inp, out)
        k, _ = run_json([dict_tool, "peek-sarc", archive, inp, PEEK], logs / f"{tag}-k.log")
        row.update(bytes=archive.stat().st_size, sha256=r4.digest(archive), compress_wall_ms=c["call_wall_ms"],
                   compress_cpu_ms=c["call_cpu_ms"], extract_wall_ms=x["call_wall_ms"], extract_cpu_ms=x["call_cpu_ms"],
                   peak_compress_bytes=c["peak_working_set_bytes"], peak_extract_bytes=x["peak_working_set_bytes"],
                   peek_median_ms=k["median_ms"], peek_p90_ms=k["p90_ms"],
                   parts={"header": c["header_bytes"], "index": c["index_bytes"], "data": c["data_bytes"]},
                   note="production path: compress includes per-block self-check and durable commit; extract writes files")
        shutil.rmtree(out)
        archive.unlink()
    else:
        cont = work / f"{tag}.sdx"
        p, row["compress_process_ms"] = run_json([dict_tool, "pack", inp, cont, cfg["level"], cfg["dict"], cfg["train"]], logs / f"{tag}-c.log")
        v, _ = run_json([dict_tool, "verify", cont, inp], logs / f"{tag}-v.log")
        k, _ = run_json([dict_tool, "peek", cont, inp, PEEK], logs / f"{tag}-k.log")
        row["byte_exact"] = bool(v["byte_exact"] and k["byte_exact"])
        row.update(bytes=p["container_bytes"], sha256=r4.digest(cont), parts=p["parts"], dictionary=p["dictionary"],
                   training_samples=p["training_samples"], training_input_bytes=p["training_input_bytes"],
                   read_wall_ms=p["read_wall_ms"], train_wall_ms=p["train_wall_ms"], train_cpu_ms=p["train_cpu_ms"],
                   encode_wall_ms=p["encode_wall_ms"], encode_cpu_ms=p["encode_cpu_ms"],
                   compress_wall_ms=p["call_wall_ms"], compress_cpu_ms=p["call_cpu_ms"],
                   extract_wall_ms=v["decode_wall_ms"], extract_cpu_ms=v["decode_cpu_ms"],
                   peak_compress_bytes=p["peak_memory_bytes"], peak_extract_bytes=v["peak_memory_bytes"],
                   peak_metric=p["peak_memory_metric"], peek_median_ms=k["median_ms"], peek_p90_ms=k["p90_ms"],
                   per_file=p["per_file"],
                   note="research container: compress = read + train + encode (no durable commit); extract = in-memory decode")
        cont.unlink()
    assert row["byte_exact"], f"round trip failed: {tag}"
    return row


def stats(values):
    v = sorted(values)
    q = lambda p: v[min(len(v) - 1, round(p * (len(v) - 1)))]
    return {"n": len(v), "mean": statistics.fmean(v), "median": statistics.median(v), "p10": q(0.1), "p90": q(0.9),
            "min": v[0], "max": v[-1]}


def analyze(run):
    raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
    cfg = json.loads((run / "config.json").read_text(encoding="utf-8"))
    rows = {}
    for r in raw:
        rows.setdefault((r["dataset"], r["config"]), []).append(r)
    summary = {}
    for (d, c), g in sorted(rows.items()):
        assert len({x["sha256"] for x in g}) == 1, f"nondeterministic {d} {c}"
        first = g[0]
        summary[f"{d}|{c}"] = {
            "dataset": d, "config": c, "kind": first["kind"], "bytes": first["bytes"], "parts": first["parts"],
            "repetitions": len(g), "byte_exact": all(x["byte_exact"] for x in g),
            "dictionary": first.get("dictionary"), "training_input_bytes": first.get("training_input_bytes"),
            **{k: stats([x[k] for x in g]) for k in ("compress_wall_ms", "compress_cpu_ms", "extract_wall_ms",
                                                    "extract_cpu_ms", "peek_median_ms", "peek_p90_ms")
               if all(x.get(k) is not None for x in g)},
            **({k: stats([x[k] for x in g]) for k in ("train_wall_ms", "encode_wall_ms", "read_wall_ms")}
               if first["kind"] == "container" else {}),
            "peak_compress_bytes": max(x["peak_compress_bytes"] or 0 for x in g),
            "peak_extract_bytes": max(x["peak_extract_bytes"] or 0 for x in g)}
    # Per-file stored bytes vs the same-level no-dictionary container (data only; index identical).
    files = {}
    for (d, c), g in rows.items():
        if g[0]["kind"] == "container":
            files[(d, c)] = {p: s for p, _, s in g[0]["per_file"]}
    per_file = {}
    for (d, c), f in files.items():
        if "nodict" in c:
            continue
        base = files[(d, f"C-nodict-{c.split('-')[2]}")]
        delta = {p: base[p] - f[p] for p in base}
        vals = list(delta.values())
        per_file[f"{d}|{c}"] = {"saving_bytes": stats(vals), "positive": sum(v > 0 for v in vals),
                                "zero": sum(v == 0 for v in vals), "negative": sum(v < 0 for v in vals),
                                "worst": sorted(([p, v] for p, v in delta.items() if v < 0), key=lambda x: x[1])[:10]}
    result = {"phase": cfg["phase"], "summary": summary, "per_file_vs_nodict": per_file}
    if cfg["phase"] == "dev":
        selection = {}
        for level in LEVELS:
            cands = {}
            for key, s in summary.items():
                d, c = key.split("|")
                if s["kind"] == "container" and c.startswith(f"C-dict-{level}-"):
                    cands.setdefault(c, []).append(s)
            best = min(cands.items(), key=lambda kv: (sum(x["bytes"] for x in kv[1]),
                                                       int(kv[0].split("-D")[1].split("k")[0]),
                                                       kv[0].split("-T")[1]))
            name = best[0]
            d_kib = int(name.split("-D")[1].split("k")[0])
            t = name.split("-T")[1]
            selection[str(level)] = {"config": name, "dict": d_kib * 1024, "train": 0 if t == "all" else int(t) << 20,
                                     "dev_total_bytes": sum(x["bytes"] for x in best[1])}
        result["selection"] = selection
        r4.save(run / "selection.json", {"rule": "per level: minimum sum of dev container bytes; ties: smaller D, then T",
                                         "selection": selection})
    r4.save(run / "summary.json", result)
    preview(run, cfg, result)


def preview(run, cfg, result):
    s = result["summary"]
    datasets = sorted({v["dataset"] for v in s.values()})
    configs_ = sorted({v["config"] for v in s.values()}, key=lambda c: (c[0] != "P", c))
    f = lambda v, n=1: "—" if v is None else f"{v:,.{n}f}"
    L = [f"# DEV-013 shared-dictionary study — {cfg['phase']} split (generated from raw.json)", "",
         f"{len(json.loads((run / 'raw.json').read_text(encoding='utf-8')))} runs, all byte-exact; archives / containers identical across repetitions.",
         "Sizes are complete artifacts (container: header + dictionary + group table + index + data). Times: medians.", ""]
    L += ["## Size (bytes)", "", "| config | " + " | ".join(datasets) + " | total |", "| --- |" + " ---: |" * (len(datasets) + 1)]
    for c in configs_:
        vals = [s.get(f"{d}|{c}", {}).get("bytes") for d in datasets]
        if None in vals:
            continue
        L.append(f"| {c} | " + " | ".join(f"{v:,}" for v in vals) + f" | {sum(vals):,} |")
    L += ["", "## Time and memory (median ms; peak MiB)", "",
          "| dataset | config | compress wall | compress CPU | train wall | encode wall | extract wall | single-file median / p90 | peak c / x MiB | dict B | index B |",
          "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: |"]
    for d in datasets:
        for c in configs_:
            v = s.get(f"{d}|{c}")
            if not v:
                continue
            g = lambda k: v[k]["median"] if k in v else None
            L.append(f"| {d} | {c} | {f(g('compress_wall_ms'))} | {f(g('compress_cpu_ms'))} | {f(g('train_wall_ms'))} | "
                     f"{f(g('encode_wall_ms'))} | {f(g('extract_wall_ms'))} | {f(g('peek_median_ms'), 2)} / {f(g('peek_p90_ms'), 2)} | "
                     f"{v['peak_compress_bytes'] / 2**20:.1f} / {v['peak_extract_bytes'] / 2**20:.1f} | "
                     f"{v['parts'].get('dictionary', 0):,} | {v['parts']['index']:,} |")
    L += ["", "## Per-file stored bytes vs same-level no-dictionary container", "",
          "| dataset | config | files +/0/− | mean | median | p10 | p90 | worst |", "| --- | --- | --- | ---: | ---: | ---: | ---: | --- |"]
    for k, v in sorted(result["per_file_vs_nodict"].items()):
        d, c = k.split("|")
        st = v["saving_bytes"]
        L.append(f"| {d} | {c} | {v['positive']}/{v['zero']}/{v['negative']} | {f(st['mean'])} | {f(st['median'], 0)} | "
                 f"{f(st['p10'], 0)} | {f(st['p90'], 0)} | " + (", ".join(f"{p} ({x:+})" for p, x in v["worst"][:2]) or "—") + " |")
    if "selection" in result:
        L += ["", "## Selection (registered rule)", ""] + [f"- level {k}: {v['config']} (dev total {v['dev_total_bytes']:,} B)"
                                                           for k, v in result["selection"].items()]
    (run / "preview.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--phase", choices=["dev", "test"])
    ap.add_argument("--run-id")
    ap.add_argument("--selection", type=Path, help="selection.json from the dev run (test phase)")
    ap.add_argument("--repetitions", type=int)
    ap.add_argument("--rerun-reason", default=None)
    ap.add_argument("--analyze-only", type=Path)
    a = ap.parse_args()
    if a.analyze_only:
        analyze(a.analyze_only.resolve(strict=True))
        return
    if not a.phase or not a.run_id or (a.phase == "test" and not a.selection):
        ap.error("need --phase, --run-id and (test) --selection")
    reps = a.repetitions or (3 if a.phase == "dev" else 5)
    run, local = ROOT / "research/experiments" / a.run_id, ROOT / "research/local" / a.run_id
    if run.exists() or local.exists():
        ap.error("run exists; use a new run ID")
    if a.phase == "test":
        previous = sorted(p.name for p in (ROOT / "research/experiments").glob("*-dict-test-*"))
        if previous and not a.rerun_reason:
            ap.error(f"test split already evaluated ({previous}); a rerun needs --rerun-reason")
    disjoint = corpus.check_disjoint()
    split_root = ROOT / "research/local" / CORPUS / a.phase
    for pool in POOLS:
        rows = corpus.read_listing(ROOT / "research/datasets" / CORPUS / f"{a.phase}-{pool}.tsv")
        snap = r4.snapshot(split_root / pool)
        assert {k: (v["bytes"], v["sha256"]) for k, v in snap.items()} == {r: (s, h) for r, s, h, _ in rows}, pool
    rel = Path("build/msvc-vs2026-release/research/tools/Release")
    (run / "logs").mkdir(parents=True)
    local.mkdir(parents=True)
    tools = []
    for name in ("seedarc_dict_study.exe", "seedarc_seed_archive.exe"):
        shutil.copyfile(ROOT / rel / name, local / name)
        tools.append(local / name)
    for script in ("b2_dictionary.py", "b2_corpus_r2.py", "b2_diagnose.py"):
        shutil.copyfile(Path(__file__).with_name(script), run / script)
    selection = json.loads(a.selection.read_text(encoding="utf-8"))["selection"] if a.selection else None
    cfgs = configs(a.phase, selection)
    git = lambda *x: subprocess.check_output(["git", "-C", str(ROOT), *x], text=True).strip()
    config = {"status": "running", "run_id": a.run_id, "phase": a.phase, "corpus": CORPUS, "disjointness": disjoint,
              "repetitions": reps, "configs": [label(c) for c in cfgs], "peek_samples": PEEK, "chunk_bytes": 65536,
              "selection_input": str(a.selection) if a.selection else None, "selection": selection,
              "rerun_reason": a.rerun_reason, "registered_in": "research/ideas/shared-dictionary-r1.md",
              "cache": "warm: each dataset read once before measurement", "order": "config order rotated per repetition",
              "tools_sha256": {t.name: r4.digest(t) for t in tools}, "script_sha256": r4.digest(Path(__file__)),
              "git_commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
              "started_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(), "os": platform.platform(),
              "cpu": platform.processor(), "logical_cpus": os.cpu_count(), "python": sys.version,
              "invocation": [sys.executable, *sys.argv],
              "codec_versions": subprocess.check_output([str(tools[1]), "--version"], text=True).strip()}
    r4.save(run / "config.json", config)
    raw = []
    try:
        for pool in POOLS:
            r4.snapshot(split_root / pool)
        for rep in range(reps):
            order = cfgs[rep % len(cfgs):] + cfgs[:rep % len(cfgs)]
            for pool in POOLS:
                for cfg in order:
                    tag = f"{rep}-{pool}-{label(cfg)}"
                    row = one(tools, cfg, split_root / pool, local, run / "logs", tag)
                    row.update(dataset=f"{a.phase}/{pool}", repeat=rep)
                    if rep != 0:
                        row.pop("per_file", None)
                    raw.append(row)
                    print(tag, row["bytes"], flush=True)
            r4.save(run / "raw.json", raw)
        r4.save(run / "raw.json", raw)
        analyze(run)
        config["status"] = "completed"
    except Exception as error:
        config["status"], config["error"] = "failed", str(error)
        r4.save(run / "raw.json", raw)
        raise
    finally:
        config["finished_at"] = datetime.now(ZoneInfo("America/Los_Angeles")).isoformat()
        r4.save(run / "config.json", config)


if __name__ == "__main__":
    main()
