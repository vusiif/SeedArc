#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate saved B2-r2 observations and produce a compact Markdown report (stdlib)."""
import json
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def report(data):
    require(data["schema"] == "seedarc.file-contract.r2" and data["plan_only"] is True,
            "unexpected schema")
    require(data["catalog_revision"] == 1 and data["overlay_revision"] == 1
            and data["base_candidate"] == {"id": 2, "revision": 1}, "unexpected rules")
    require(data["config"] == {"codec_seed": 0, "base_zstd_rank": 2,
                              "global_zstd_rank": 3, "file_zstd_rank": 5,
                              "lzma2_rank": 6}, "unexpected fields")
    rows = ["# B2-r2 文件身份与叠加对照", "",
            "纯计划观察；未压缩、未搜索，不代表收益或性能。", "",
            "| 场景 | 块数 | 文件覆盖改变的块（相对当前全局） | 解释 |",
            "| --- | ---: | --- | --- |"]
    scenarios = data["scenarios"]
    require([s["name"] for s in scenarios] == ["combined", "insert-reorder",
            "delete-reindex", "empty.txt", "single.txt", "large.txt"], "missing scenarios")
    for scenario in scenarios:
        files = scenario["files"]
        paths = {f["file_id"]: f["path"] for f in files}
        require(len(paths) == len(files) == len(set(paths.values())), "duplicate file")
        require(scenario["target_path"] in paths.values(), "unknown target")
        blocks = scenario["blocks"]
        ids = [b["global_block_id"] for b in blocks]
        require(ids == sorted(set(ids)), "noncanonical IDs")
        require(len({(b["file_id"], b["file_block_index"]) for b in blocks}) == len(blocks),
                "duplicate local identity")
        changes = []
        for block in blocks:
            require(block["file_id"] in paths, "unknown block owner")
            base = block["base"]
            codec = base["codec"]
            require(codec in ("store", "zstd", "lzma2"), "unknown codec")
            require(base["level"] == {"store": 0, "zstd": 3, "lzma2": 6}[codec],
                    "unexpected base")
            global_plan = {"codec": codec, "level": 4 if codec == "zstd" else base["level"]}
            selected = paths[block["file_id"]] == scenario["target_path"]
            file_plan = {"codec": codec, "level": 6 if selected and codec == "zstd"
                         else global_plan["level"]}
            require(block["global"] == global_plan and block["file"] == file_plan
                    and block["cleared"] == global_plan, "incorrect overlay/clear semantics")
            if file_plan != global_plan:
                changes.append(block["global_block_id"])
        require(scenario["file_changes_from_global"] == len(changes), "incorrect change count")
        expected_effect = "target_codec_levels_changed" if changes else "no_target_codec_blocks"
        require(scenario["effect"] == expected_effect, "incorrect effect description")
        if scenario["name"] in ("empty.txt", "single.txt"):
            require(not changes, "expected no change")
        else:
            require(changes, "vacuous affected-file observation")
        rows.append(f'| {scenario["name"]} | {len(blocks)} | '
                    f'{", ".join(map(str, changes)) or "无"} | '
                    f'{"无目标 Codec 块，成功且无变化" if not changes else "仅目标文件 Zstd 等级变化"} |')
    initial = {b["global_block_id"]: b for b in scenarios[0]["blocks"]}
    inserted = {b["global_block_id"]: b for b in scenarios[1]["blocks"]}
    require(all(inserted[i] == b for i, b in initial.items()), "insertion moved original plans")
    require(data["stale_path_checks"] == {"renamed": "invalid_argument",
                                          "removed": "invalid_argument"}, "stale key accepted")
    rows += ["", "## 叠加与清除", "",
             "目标文件 dir/b.txt 的 Zstd 块5/6：**3 → 4 → 6 → 4**。",
             "其他文件的 Zstd 块1/3/8：3 → 4 → 4 → 4；Codec 不变。", "",
             "插入 / 重排保留原 global ID 时，原12块计划完全相同。删除其他文件并重编号后，",
             "路径覆盖仍只匹配 dir/b.txt；Codec 可随 global ID 变化。", "",
             "目标删除或重命名后旧覆盖明确失败，不会迁移到其他文件。空文件是显式清单项，",
             "合法编辑成功但无块变化；single.txt 只有 LZMA2 块，修改 Zstd 同样无变化。", "",
             "128块大文件的所有目标 Zstd 块均受覆盖；清除后继承当前全局4。",
             "另有10000块单文件单测，未测量峰值内存或宣称生成性能。"]
    return "\n".join(rows) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: file_contract_table.py observation.json")
    print(report(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))), end="")
