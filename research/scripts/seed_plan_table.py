# SPDX-License-Identifier: GPL-3.0-or-later
"""Print a Markdown comparison from two saved seed-plan JSON outputs; no writes."""

import argparse
import json
from pathlib import Path


def load(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "seedarc.seed-plan.r1" or not data.get("plan_only"):
        raise ValueError("expected a revision-1 plan-only observation")
    rows = data["blocks"]
    ids = [row["block_id"] for row in rows]
    if len(ids) != len(set(ids)) or not all("compared" in row for row in rows):
        raise ValueError("expected unique block IDs and a compared plan for every block")
    return data


def label(plan):
    codec = plan["codec"]
    if codec not in ("store", "zstd", "lzma2"):
        raise ValueError("unknown codec")
    return codec if codec == "store" else f"{codec}-{int(plan['level'])}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    args = parser.parse_args()
    first, second = load(args.first), load(args.second)
    if [r["block_id"] for r in first["blocks"]] != [r["block_id"] for r in second["blocks"]]:
        raise ValueError("block sequences differ; comparing unrelated rows would be misleading")
    print("| block_id | first before | first after | second before | second after |")
    print("| --- | --- | --- | --- | --- |")
    for a, b in zip(first["blocks"], second["blocks"]):
        print(f"| {a['block_id']} | {label(a['proposed'])} | {label(a['compared'])} | "
              f"{label(b['proposed'])} | {label(b['compared'])} |")


if __name__ == "__main__":
    main()
