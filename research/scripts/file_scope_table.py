# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate saved overlay-r1 observations and print a Markdown difference table."""

import argparse
import json
from pathlib import Path


def check(condition, message):
    if not condition:
        raise ValueError(message)


def edited(plan, fields):
    result = dict(plan)
    if plan["codec"] == "zstd" and fields["zstd_rank"] is not None:
        result["level"] = fields["zstd_rank"] + 1
    elif plan["codec"] == "lzma2" and fields["lzma2_rank"] is not None:
        result["level"] = fields["lzma2_rank"]
    return result


def label(plan):
    return plan["codec"] if plan["codec"] == "store" else f"{plan['codec']}-{plan['level']}"


def render(data):
    check(data.get("schema") == "seedarc.seed-file-scope.r1" and data.get("plan_only") is True,
          "expected an overlay-r1 plan-only observation")
    rows = data["blocks"]
    override = data["config"]["file_override"]
    ids = [row["global_block_id"] for row in rows]
    check(ids == sorted(set(ids)), "duplicate or noncanonical global block IDs")
    changed = {key: [] for key in ("global_edit", "file_edit", "cleared", "clear_from_file_edit")}
    selected_target_blocks = 0
    other_target_blocks = 0
    for row in rows:
        base = row["base"]
        selected = row["file_id"] == override["file_id"]
        expected = edited(base, override)
        check(row["global_edit"] == expected, "global rank edit changed unrelated fields")
        check(row["file_edit"] == (expected if selected else base),
              "file edit changed the wrong scope or failed field inheritance")
        check(row["cleared"] == base, "clearing the override did not restore defaults")
        if expected != base:
            if selected:
                selected_target_blocks += 1
            else:
                other_target_blocks += 1
        for state in ("global_edit", "file_edit", "cleared"):
            if row[state] != base:
                changed[state].append(row["global_block_id"])
        if row["cleared"] != row["file_edit"]:
            changed["clear_from_file_edit"].append(row["global_block_id"])
    check(changed == data["changed_global_ids"], "declared change sets differ from actual plans")
    check(selected_target_blocks > 0 and other_target_blocks > 0,
          "observation is vacuous: both selected and unselected files must contain affected blocks")
    lines = ["# 文件级等级覆盖对照", "", "只比较拟议计划，不代表压缩或搜索收益。", "",
             f"全局编辑改变块：`{changed['global_edit']}`；文件覆盖改变块：`{changed['file_edit']}`。",
             "清除后与基础计划完全一致；对选中与未选中文件均存在真实目标块的情况已核对。", "",
             "| 文件 | 文件内块 | 全局块 | 基础 | 全局修改 | 文件覆盖 | 清除覆盖 |",
             "| --- | --- | --- | --- | --- | --- | --- |"]
    for row in rows:
        lines.append(f"| {row['file_id']} | {row['file_block_index']} | {row['global_block_id']} | "
                     + " | ".join(label(row[state]) for state in
                                  ("base", "global_edit", "file_edit", "cleared")) + " |")
    lines.extend(["", "本轮没有修改 codec 调度，没有验证跨文件插入 / 重命名的身份稳定性。",
                  "文件覆盖是独立编辑层，尚未选定如何编码进种子或用于搜索。"])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("observation", type=Path)
    args = parser.parse_args()
    print(render(json.loads(args.observation.read_text(encoding="utf-8"))), end="")


if __name__ == "__main__":
    main()
