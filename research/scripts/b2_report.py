# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate B2-r3 summary/table from archived raw.json; no compression execution."""
import json
from pathlib import Path
import runpy
import sys


def report(raw_path):
    namespace = runpy.run_path(str(Path(__file__).with_name("b2_compression.py")))
    raw = json.loads(raw_path.read_text(encoding="utf-8"))
    summary = namespace["summarize"](raw)
    for row in raw:
        target = json.loads((raw_path.parent / "datasets.json").read_text(encoding="utf-8"))[
            "datasets"][row["dataset"]]["target"]
        namespace["validate_plan"](row["plan"], row["mode"], target)
    lines = ["# B2-r3 真实压缩首轮结果", "",
             f"共 {len(raw)} 次压缩 / 验证 / 解压，均逐字节还原。",
             "输入为磁盘上的项目源码与自制样本，尚非独立真实语料评估。", "",
             "归档大小包含头部、索引与数据；未序列化 Seed / 覆盖字段。",
             "时间是进程墙钟时间，包含启动、计划生成、自检与证据输出；峰值为该压缩进程的工作集。",
             "每组重复3次，未控制OS缓存，不作统计显著性或普遍性能结论。", "",
             "| 数据 | 方案 | 完整归档 bytes | 回退块 | 压缩中位 s | 解压中位 s | 压缩峰值 MiB |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for r in summary:
        lines.append(f'| {r["dataset"]} | {r["mode"]} | {r["archive_bytes"]} | '
                     f'{r["fallback_blocks"]} | {r["median_compress_seconds"]:.4f} | '
                     f'{r["median_extract_seconds"]:.4f} | '
                     f'{r["max_compress_peak_working_set_bytes"] / 1048576:.2f} |')
    lines += ["", "## 相同 Codec 分配下的文件覆盖", "",
              "以下仅比较 b-global 与 b-file；覆盖目标在运行前选择，未试压调参。", "",
              "| 数据 | 文件覆盖相对全局归档字节变化 | 拟议等级变化块数 |",
              "| --- | ---: | ---: |"]
    for dataset in sorted({r["dataset"] for r in raw}):
        group = {r["mode"]: r for r in raw if r["dataset"] == dataset and r["repeat"] == 0}
        delta = group["b-file"]["archive_bytes"] - group["b-global"]["archive_bytes"]
        changes = sum(a["planned_config"] != b["planned_config"]
                      for a, b in zip(group["b-global"]["plan"]["blocks"], group["b-file"]["plan"]["blocks"]))
        lines.append(f"| {dataset} | {delta:+d} | {changes} |")
    lines += ["", "每组同环境重复运行的归档 SHA256 一致；b-cleared 与 b-global 归档逐字节一致。",
              "实际 Codec / 配置与拟议计划逐块核对，压缩无收益时实际索引明确记录 Store。",
              "固定单算法与 B 的 Codec 分配不同，不能把两者差异归因于文件覆盖。",
              "没有搜索预算实验、Seed编码选择或独创性结论；完整时间 / 内存数据保留在 summary.json。"]
    return summary, "\n".join(lines) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: b2_report.py raw.json")
    _, markdown = report(Path(sys.argv[1]))
    print(markdown, end="")
