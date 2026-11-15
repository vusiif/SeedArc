# SPDX-License-Identifier: GPL-3.0-or-later
"""G5 runner (DEV-016, research/ideas/seed-g5-r1.md). Standard library only.

For each pool: the G-dict-r1 start plan (`pack 0x40`), then every method x budget x search seed
(`search`), each followed by an independent `verify`. Raw tool JSON goes to raw.json, the
effective configuration to config.json and a per-cell summary to summary.json, all inside
research/experiments/<run-id>/. Containers go to the ignored research/local/g5-runs/<run-id>/.

  python -I research/scripts/g5_run.py --tool <seedarc_g5_study> --run-id 20261114-g5-dev-main-001 \
      --phase dev --pools config-cmake-pkg --budgets 8 32 128 --seeds 1 2

--phase holdout reads g5-holdout-r1 and additionally requires --frozen-config <json>, whose
methods / budgets / seeds / stall must match the arguments (no tuning on the holdout).
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
POOLS = ["many-small-docs", "source-python", "config-cmake-pkg"]


HOLDOUT = ROOT / "research/local/g5-holdout-r1"


def inputs(phase, pool):
    """[(subset, directory)]: dev uses the whole pool; the holdout also reports each half."""
    if phase == "dev":
        return [("all", ROOT / "research/local/b2r5-corpus-r2/dev" / pool)]
    out = [("all", HOLDOUT / "files" / pool)]
    for half in ("local", "public"):
        target = HOLDOUT / "halves" / half / pool
        if not target.exists():
            listing = ROOT / "research/datasets/g5-holdout-r1" / f"{half}-{pool}.tsv"
            for line in listing.read_text(encoding="utf-8").splitlines():
                rel, size, sha = line.split("\t")[:3]
                data = (HOLDOUT / "files" / pool / rel).read_bytes()
                if len(data) != int(size) or hashlib.sha256(data).hexdigest() != sha:
                    raise RuntimeError(f"holdout file differs from its listing: {rel}")
                dest = target / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
        out.append((half, target))
    return out


def files_of(path):
    return {p.relative_to(path).as_posix(): p.read_bytes() for p in sorted(path.rglob("*")) if p.is_file()}


def input_bytes(path):
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def lzma_reference(cli, src, work):
    """Production archive with LZMA2-6: compress -> verify -> extract -> byte compare."""
    archive, out = work / "lzma2-6.sarc", work / "lzma2-6-out"
    t0 = time.perf_counter()
    c = subprocess.run([str(cli), "compress", str(src), "-o", str(archive), "--codec", "lzma2", "--level", "6"],
                       capture_output=True)
    compress_ms = (time.perf_counter() - t0) * 1000
    v = subprocess.run([str(cli), "verify", str(archive)], capture_output=True)
    t1 = time.perf_counter()
    x = subprocess.run([str(cli), "extract", str(archive), "-o", str(out)], capture_output=True)
    extract_ms = (time.perf_counter() - t1) * 1000
    if c.returncode or v.returncode or x.returncode:
        raise RuntimeError(f"lzma2-6 reference failed: {c.stderr[:200]} {v.stderr[:200]} {x.stderr[:200]}")
    return {"archive_bytes": archive.stat().st_size, "compress_process_ms": compress_ms,
            "extract_process_ms": extract_ms, "byte_exact": files_of(src) == files_of(out),
            "cli_stdout": c.stdout.decode(errors="replace").strip()}


def run(cmd, timeout=7200):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f"exit {p.returncode}: {' '.join(map(str, cmd[1:3]))}: "
                           f"{p.stderr.decode(errors='replace')[:400]}")
    return json.loads(p.stdout)


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True)
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--phase", choices=["dev", "holdout"], required=True)
    ap.add_argument("--pools", nargs="+", default=POOLS, choices=POOLS)
    ap.add_argument("--methods", nargs="+", default=["random", "coord", "seed"])
    ap.add_argument("--budgets", nargs="+", type=int, required=True, help="multiples of the pool's input bytes")
    ap.add_argument("--seeds", nargs="+", type=int, required=True)
    ap.add_argument("--stall", type=int, default=20)
    ap.add_argument("--frozen-config")
    ap.add_argument("--cli", help="production seedarc CLI for the LZMA2-6 reference")
    a = ap.parse_args()
    tool = Path(a.tool).resolve(strict=True)
    cli = Path(a.cli).resolve(strict=True) if a.cli else None
    if a.phase == "holdout" and cli is None:
        ap.error("--phase holdout needs --cli (LZMA2-6 reference)")
    exp = ROOT / "research/experiments" / a.run_id
    local = ROOT / "research/local/g5-runs" / a.run_id
    if exp.exists() or local.exists():
        ap.error("run directory exists; use a new run ID")
    if a.phase == "holdout":
        if not a.frozen_config:
            ap.error("--phase holdout needs --frozen-config")
        frozen = json.loads(Path(a.frozen_config).read_text(encoding="utf-8"))
        for key in ("methods", "budgets", "seeds", "stall"):
            if frozen[key] != getattr(a, key):
                ap.error(f"{key} differs from the frozen configuration")
    exp.mkdir(parents=True)
    local.mkdir(parents=True)
    config = {"run_id": a.run_id, "phase": a.phase, "started_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
              "command": [sys.executable, "-I", *sys.argv], "tool": str(tool.name),
              "tool_sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
              "git_commit": git("rev-parse", "HEAD"), "git_dirty": git("status", "--porcelain") != "",
              "machine": {"os": platform.platform(), "processor": platform.processor(), "python": platform.python_version()},
              "pools": a.pools, "methods": a.methods, "budgets": a.budgets, "seeds": a.seeds, "stall": a.stall,
              "budget_unit": "multiples of the pool's input bytes; counts training + trial-encoding input bytes of uncached groups",
              "level": 9, "chunk_bytes": 65536, "corpus": "b2r5-corpus-r2 dev" if a.phase == "dev" else "g5-holdout-r1"}
    (exp / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8", newline="\n")
    raw, references = [], []
    for pool in a.pools:
        for subset, src in inputs(a.phase, pool):
            tag = f"{pool}-{subset}"
            size = input_bytes(src)
            if cli is not None:
                (local / tag).mkdir()
                ref = lzma_reference(cli, src, local / tag)
                references.append({"pool": pool, "subset": subset, "input_bytes": size, "lzma2_6": ref})
                print(tag, "lzma2-6", ref["archive_bytes"], ref["byte_exact"], flush=True)
            start = run([tool, "pack", "0x40", src, local / f"{tag}-start.sadx"])
            check = run([tool, "verify", local / f"{tag}-start.sadx", src])
            raw.append({"pool": pool, "subset": subset, "input_bytes": size, "method": "start", "budget_x": 0,
                        "seed": None, "result": start, "verify": check})
            print(tag, "start", start["container_bytes"], flush=True)
            for method in a.methods:
                for bx in a.budgets:
                    for seed in a.seeds:
                        out = local / f"{tag}-{method}-b{bx}-s{seed}.sadx"
                        r = run([tool, "search", method, src, bx * size, seed, out, a.stall])
                        v = run([tool, "verify", out, src])
                        raw.append({"pool": pool, "subset": subset, "input_bytes": size, "method": method,
                                    "budget_x": bx, "seed": seed, "result": r, "verify": v})
                        print(tag, method, bx, seed, r["container_bytes"], r["plan_evaluations"],
                              round(r["search_wall_ms"]), flush=True)
    (exp / "raw.json").write_text(json.dumps(raw, indent=1) + "\n", encoding="utf-8", newline="\n")
    if references:
        (exp / "references.json").write_text(json.dumps(references, indent=1) + "\n", encoding="utf-8", newline="\n")
    summary = []
    for pool, subset in dict.fromkeys((x["pool"], x["subset"]) for x in raw):
        cell = [x for x in raw if x["pool"] == pool and x["subset"] == subset]
        start = next(x for x in cell if x["method"] == "start")["result"]["container_bytes"]
        lzma = next((r["lzma2_6"]["archive_bytes"] for r in references
                     if r["pool"] == pool and r["subset"] == subset), None)
        for method in a.methods:
            for bx in a.budgets:
                rows = [x for x in cell if x["method"] == method and x["budget_x"] == bx]
                sizes = sorted(x["result"]["container_bytes"] for x in rows)
                summary.append({"pool": pool, "subset": subset, "method": method, "budget_x": bx, "runs": len(rows),
                                "lzma2_6_bytes": lzma,
                                "start_bytes": start, "min": sizes[0], "median": statistics.median(sizes), "max": sizes[-1],
                                "median_vs_start_pct": round(100 * (statistics.median(sizes) - start) / start, 3),
                                "median_plan_evaluations": statistics.median(x["result"]["plan_evaluations"] for x in rows),
                                "median_search_wall_ms": round(statistics.median(x["result"]["search_wall_ms"] for x in rows)),
                                "all_byte_exact": all(x["result"]["byte_exact"] and x["verify"]["byte_exact"] for x in rows)})
    (exp / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8", newline="\n")
    config["finished_at"] = datetime.now(ZoneInfo("America/Los_Angeles")).isoformat()
    (exp / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8", newline="\n")
    for s in summary:
        print(s)


if __name__ == "__main__":
    main()
