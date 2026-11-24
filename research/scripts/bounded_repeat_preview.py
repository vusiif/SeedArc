# SPDX-License-Identifier: GPL-3.0-or-later
"""Render min/median/max and within-round comparisons without significance claims."""
import json
from pathlib import Path
import sys


def render(a):
    def triple(s):return f"{s['min']:.3f} / {s['median']:.3f} / {s['max']:.3f}"
    cases=a['summaries']
    fresh=[x for x in cases if x['operation']=='fresh']
    text=f"""# 有界固实：平衡轮换复测

每组{a['rounds']}次同一dev / 热缓存条件观测；配置位置循环轮换，重建 / 追加先后交替。父包准备成本单列，产物逐字节往返且SHA与DEV-021参考一致。首轮单遍数据不混入统计。数字按min / median / max列出，不提供统计显著性或跨数据保证。

## 全量重建及单文件读取 / 解码

| 帧上限 | 完整包 MB | 比整段体积 | 编码调用 s | 提取诊断中位 ms | 同轮提取倍率 |
| --- | ---: | ---: | ---: | ---: | ---: |
"""
    for s in fresh:
        label='整段（1GiB资源上限）' if s['cap']==1024**3 else f"{s['cap']/1024**2:g} MiB"
        text+=f"| {label} | {s['bytes']/1e6:.3f} | {s['size_overhead_percent']:+.3f}% | {triple(s['call_seconds'])} | {triple(s['access_median_ms'])} | {triple(s['paired_access_speedup'])}× |\n"
    text+='''
提取诊断每次加载整个归档、解析、解码目标帧并验证，没有写出目标文件；固定均匀路径并加入最大文件。上列是每次profile中位数的跨轮描述，逐路径与P95见analysis.json / 原始JSON。倍率用同一轮整段对照除以该配置，不把不同轮拼成最有利比较。

## 从固定第3代父包追加末批新文件

| 帧上限 | 最终包 MB | 比同档重建体积 | 追加调用 s | 同档重建调用 s | 父包准备编码总 s |
| --- | ---: | ---: | ---: | ---: | ---: |
'''
    for s in [x for x in cases if x['operation']=='append']:
        f=next(x for x in fresh if x['cap']==s['cap'])
        prep=sum(x['call_seconds'] for x in a['preparation'] if x['cap']==s['cap'])
        label='整段对照' if s['cap']==1024**3 else f"{s['cap']/1024**2:g} MiB"
        text+=f"| {label} | {s['bytes']/1e6:.3f} | {(s['bytes']/f['bytes']-1)*100:+.3f}% | {triple(s['call_seconds'])} | {triple(f['call_seconds'])} | {prep:.3f} |\n"
    text+='''
每次追加使用同一未修改的父包，旧原文重压0，但旧压缩包完整读入、复制和写出；最终包保留旧索引开销。父包准备含首次与前两批的编码调用，不是免费生命周期，不含研究用外部verify。没有用重复追加构造不同历史，也没有计算未实测的逐批全量重建累计成本。

本轮只复测时序与确定性：显式计划 / 故障诊断沿用DEV-021同C++ / 同包证据，没有新增损坏 / 缺卷恢复结论；仅Seed路径重复计时，不产生Seed独立优势结论。MB为十进制，MiB为二进制。仍需更多数据覆盖，研究选项不等于正式产品默认或性能保证。
'''
    return text


if __name__=='__main__':
    a=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
    Path(sys.argv[2]).write_text(render(a),encoding='utf-8')
