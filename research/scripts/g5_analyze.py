# SPDX-License-Identifier: GPL-3.0-or-later
"""Analyse a G5 run against the hypotheses registered in research/configs/g5-r1.frozen.json.

  python -I research/scripts/g5_analyze.py RUN_DIR [--cli seedarc] [--recheck-tool g5_study]

Reads raw.json / references.json / config.json and writes analysis.json and analysis.md next to
them. Rules (as registered, applied mechanically):
  H0  every artefact is byte exact (search self-check and independent verify), and a re-run of a
      sample of cells (--recheck-tool) reproduces identical container bytes;
  H1  at 4x, the coord median is at least 1 % below the start plan, in at least two pools;
  H2  at the same budget the seed median (Seed bytes included) is below the coord median, or the
      seed median reaches coord's best container at a smaller budget; in at least two pools.
      Primary budget 4x (as H1); every budget is reported.
Pools are the whole pools (subset "all"); the halves are reported, not used for the verdicts.

With --cli, the LZMA2-6 comparison is also given "production-index adjusted": the research
container's header, group table and index are replaced by the production archive's
header + index size, measured as (store archive bytes - input bytes) for the same input.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def med(values):
    return statistics.median(values)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--cli")
    ap.add_argument("--recheck-tool")
    a = ap.parse_args()
    run = Path(a.run)
    raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
    refs = json.loads((run / "references.json").read_text(encoding="utf-8")) if (run / "references.json").exists() else []
    config = json.loads((run / "config.json").read_text(encoding="utf-8"))
    budgets, methods = config["budgets"], config["methods"]
    cells = list(dict.fromkeys((x["pool"], x["subset"]) for x in raw))

    h0_exact = all(x["result"]["byte_exact"] and x["verify"]["byte_exact"] for x in raw)
    h0_refs = all(r["lzma2_6"]["byte_exact"] for r in refs)

    def subset_dir(pool, subset):
        base = ROOT / "research/local/g5-holdout-r1"
        if config["phase"] == "dev":
            return ROOT / "research/local/b2r5-corpus-r2/dev" / pool
        return base / "files" / pool if subset == "all" else base / "halves" / subset / pool

    recheck = []
    if a.recheck_tool:
        tool = Path(a.recheck_tool).resolve(strict=True)
        local = ROOT / "research/local/g5-runs" / config["run_id"]
        picks = [x for x in raw if x["method"] != "start" and x["seed"] == 3 and x["budget_x"] in (4, 16)
                 and x["subset"] == "all"]
        with tempfile.TemporaryDirectory() as tmp:
            for x in picks:
                src = subset_dir(x["pool"], x["subset"])
                out = Path(tmp) / "again.sadx"
                subprocess.run([str(tool), "search", x["method"], str(src), str(x["budget_x"] * x["input_bytes"]),
                                str(x["seed"]), str(out), str(config["stall"])], check=True, capture_output=True)
                first = local / f"{x['pool']}-{x['subset']}-{x['method']}-b{x['budget_x']}-s{x['seed']}.sadx"
                same = hashlib.sha256(out.read_bytes()).digest() == hashlib.sha256(first.read_bytes()).digest()
                recheck.append({"pool": x["pool"], "method": x["method"], "budget_x": x["budget_x"], "identical": same})
                out.unlink()

    overhead = {}
    if a.cli:
        cli = Path(a.cli).resolve(strict=True)
        with tempfile.TemporaryDirectory(dir=ROOT / "research/local") as tmp:
            for pool, subset in cells:
                src = subset_dir(pool, subset)
                archive = Path(tmp) / f"{pool}-{subset}.sarc"
                subprocess.run([str(cli), "compress", str(src), "-o", str(archive), "--codec", "store"],
                               check=True, capture_output=True)
                size = sum(p.stat().st_size for p in src.rglob("*") if p.is_file())
                overhead[(pool, subset)] = archive.stat().st_size - size

    table = []
    for pool, subset in cells:
        cell = [x for x in raw if x["pool"] == pool and x["subset"] == subset]
        start = next(x for x in cell if x["method"] == "start")
        lzma = next((r["lzma2_6"]["archive_bytes"] for r in refs if r["pool"] == pool and r["subset"] == subset), None)
        row = {"pool": pool, "subset": subset, "input_bytes": start["input_bytes"],
               "lzma2_6": lzma, "start": start["result"]["container_bytes"], "methods": {}}
        prod_overhead = overhead.get((pool, subset))
        for method in methods:
            row["methods"][method] = {}
            for bx in budgets:
                rs = [x for x in cell if x["method"] == method and x["budget_x"] == bx]
                sizes = [x["result"]["container_bytes"] for x in rs]
                m = med(sizes)
                entry = {"median": m, "min": min(sizes), "max": max(sizes),
                         "vs_start_pct": round(100 * (m - row["start"]) / row["start"], 3),
                         "median_search_wall_ms": round(med(x["result"]["search_wall_ms"] for x in rs))}
                if lzma:
                    entry["vs_lzma_direct_pct"] = round(100 * (m - lzma) / lzma, 3)
                if lzma and prod_overhead is not None:
                    best = min(rs, key=lambda x: abs(x["result"]["container_bytes"] - m))["result"]
                    research_overhead = best["parts"]["header"] + best["parts"]["seed"] + best["parts"]["group_table"] + best["parts"]["index"]
                    adjusted = m - research_overhead + prod_overhead + best["parts"]["seed"]
                    entry["vs_lzma_adjusted_pct"] = round(100 * (adjusted - lzma) / lzma, 3)
                row["methods"][method][str(bx)] = entry
        table.append(row)

    pools = [r for r in table if r["subset"] == "all"]
    h1 = {r["pool"]: r["methods"]["coord"]["4"]["median"] <= 0.99 * r["start"] for r in pools}

    def h2_pool(r, bx):
        seed, coord = r["methods"]["seed"][str(bx)]["median"], r["methods"]["coord"][str(bx)]["median"]
        return seed < coord

    def reach(r, method, target):
        for bx in budgets:
            if r["methods"][method][str(bx)]["median"] <= target:
                return bx
        return None

    h2 = {}
    for r in pools:
        coord_best = min(r["methods"]["coord"][str(b)]["min"] for b in budgets)
        h2[r["pool"]] = {"seed_below_coord_at_4x": h2_pool(r, 4),
                         "seed_below_coord_by_budget": {str(b): h2_pool(r, b) for b in budgets},
                         "coord_best": coord_best,
                         "budget_seed_median_reaches_coord_best": reach(r, "seed", coord_best + 4),
                         "budget_coord_median_reaches_coord_best": reach(r, "coord", coord_best)}
        s, c = h2[r["pool"]]["budget_seed_median_reaches_coord_best"], h2[r["pool"]]["budget_coord_median_reaches_coord_best"]
        h2[r["pool"]]["reaches_with_less_budget"] = s is not None and (c is None or s < c)
        h2[r["pool"]]["holds"] = h2[r["pool"]]["seed_below_coord_at_4x"] or h2[r["pool"]]["reaches_with_less_budget"]
    verdict = {"H0": h0_exact and h0_refs and all(x["identical"] for x in recheck),
               "H0_detail": {"artefacts_byte_exact": h0_exact, "lzma_references_byte_exact": h0_refs,
                             "recheck": recheck, "recheck_count": len(recheck)},
               "H1": sum(h1.values()) >= 2, "H1_by_pool": h1,
               "H2": sum(v["holds"] for v in h2.values()) >= 2, "H2_by_pool": h2}
    out = {"run": config["run_id"], "phase": config["phase"], "verdict": verdict, "table": table,
           "production_overhead_bytes": {f"{p}/{s}": v for (p, s), v in overhead.items()}}
    (run / "analysis.json").write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8", newline="\n")

    lines = [f"# {config['run_id']} analysis (generated by research/scripts/g5_analyze.py)", "",
             f"H0 {verdict['H0']} | H1 {verdict['H1']} {h1} | H2 {verdict['H2']} "
             f"{ {p: v['holds'] for p, v in h2.items()} }", ""]
    for r in table:
        lines.append(f"## {r['pool']} / {r['subset']} (input {r['input_bytes']}, start {r['start']}, LZMA2-6 {r['lzma2_6']})")
        lines.append("| method | " + " | ".join(f"{b}x" for b in budgets) + " |")
        lines.append("| --- |" + " --- |" * len(budgets))
        for method in methods:
            cells_text = []
            for b in budgets:
                e = r["methods"][method][str(b)]
                adj = f", adj {e['vs_lzma_adjusted_pct']}%" if "vs_lzma_adjusted_pct" in e else ""
                cells_text.append(f"{e['median']} ({e['vs_start_pct']}% vs start; {e.get('vs_lzma_direct_pct')}% vs LZMA{adj})")
            lines.append(f"| {method} | " + " | ".join(cells_text) + " |")
        lines.append("")
    (run / "analysis.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("\n".join(lines[:3]))


if __name__ == "__main__":
    main()
