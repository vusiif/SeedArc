# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-045: tables from speed_matrix.py's raw.jsonl.

  python -I research/scripts/speed_matrix_report.py RAW_JSONL > preview.md

Per tree: every pack cell with workers, archive bytes, size versus the smallest cell, pack wall
clock, speed-up versus the reference cell (SeedArc dict256 whole, one thread), peak RSS, decode
time and the 90% single-file extraction. SeedArc's pack time is the process wall clock (it also
includes reading the tree and the tool's own setup, as for the external tools).
"""
import json
import sys

REF = 'seedarc solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup'


def short(cell):
    return (cell.replace('seedarc solid/lzma2-9e', 'SA 9e').replace('/g1-r0/frame-', ' ')
            .replace('/dedup', '').replace('-dict', ' dict').replace('|', '\\|'))


def main(path):
    rows = [json.loads(line) for line in open(path, encoding='utf-8')]
    trees = list(dict.fromkeys(r['tree'] for r in rows))
    for tree in trees:
        t = [r for r in rows if r['tree'] == tree]
        packs = [r for r in t if 'archive_bytes' in r and r['cell'] not in ('tar',)]
        probes = {}
        for r in t:
            if r.get('op') == 'full decode':
                probes.setdefault(r['cell'], {})['decode'] = r.get('decode_wall_s', r['wall_s'])
            elif r.get('op') == 'extract 90%':
                probes.setdefault(r['cell'], {})['x90'] = r['wall_s']
        tar_s = next((r['wall_s'] for r in t if r['cell'] == 'tar'), 0)
        inp = next(r for r in t if r['cell'] == 'input')
        smallest = min(r['archive_bytes'] for r in packs)
        ref = next((r for r in packs if r['cell'] == REF), None)
        print(f'### {tree}（{inp["files"]} 个文件，{inp["input_bytes"]:,} B；tar 阶段 {tar_s} s 已计入 xz 的时间）\n')
        print('| 格子 | 线程 | 字节 | 比最小 | 打包 s | 提速 | 峰值 MiB | 解码 s | 提取 90% s |')
        print('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
        for r in packs:
            wall = r['wall_s']
            auto = r.get('jobs_auto')
            threads = (f"auto→{auto['chosen']}（{auto['limited_by']}{'，75%' if r.get('budget_bytes') else ''}）"
                       if auto else str(r.get('threads', r.get('jobs', ''))))
            p = probes.get(r['cell'], {}) if not r.get('budget_bytes') else {}
            speed = f"{ref['wall_s'] / wall:.2f}×" if ref else ''
            print(f"| {short(r['cell'])} | {threads} | {r['archive_bytes']:,} | "
                  f"{(r['archive_bytes'] / smallest - 1) * 100:+.2f}% | {wall:.1f} | {speed} | "
                  f"{(r['peak_bytes'] or 0) / 2**20:,.0f} | {p.get('decode', '')} | {p.get('x90', '')} |")
        skipped = [r for r in t if r.get('op') == '75% budget skipped']
        if skipped:
            print('\n75% 预算未追加（按公式不能多开线程）：' + '、'.join(short(r['cell']) for r in skipped))
        print()


if __name__ == '__main__':
    main(sys.argv[1])
