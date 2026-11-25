# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-023: markdown tables from bounded_expand_report analysis.json (no new numbers)."""
import json
from pathlib import Path
import sys

MIB = 1024**2


def cap_name(c):
    return 'whole (1 GiB)' if int(c) == 1024**3 else f'{int(c) // MIB} MiB'


def mmm(d, scale=1.0, fmt='{:.0f}'):
    return '–'.join(fmt.format(d[k] * scale) for k in ['min', 'median', 'max'])


def main():
    data = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
    out = ['# DEV-023 observation tables', '',
           f"Generated from `analysis.json` by `bounded_expand_preview.py`; all audits ok: {data['all_audits_ok']}. "
           'Times are four-round min–median–max on one pinned CPU, warm cache. "Read/decode" is the '
           'access-profile diagnostic (whole archive read + target frame decode + byte check, nothing written).', '']
    for u in data['analysis']:
        out += [f"## {u['unit']} — {u['files']} files, {u['bytes']:,} B, added {u['added_bytes']:,} B ({u['added_ratio'] * 100:.2f}%)", '',
                '| Frame limit | Op | Archive B | vs whole | Frames | Encode s | Read/decode median ms | Same-round read speedup | Append/fresh wall | Append size vs fresh |',
                '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
        for c, item in u['caps'].items():
            for op in ['fresh', 'append']:
                e = item[op]
                avf = item['append_vs_fresh']
                out.append(f"| {cap_name(c)} | {op} | {e['container_bytes']:,} | {e['vs_reference_percent']:+.3f}% | {e['frame_count']} | "
                           f"{mmm(e['process_wall_ms'], 1e-3, '{:.2f}')} | {mmm(e['access_median_ms'], 1, '{:.1f}')} | "
                           f"{mmm(e['same_round_access_speedup_vs_reference'], 1, '{:.2f}')}× | "
                           + (f"{mmm(avf['same_round_wall_ratio'], 1, '{:.3f}')} | {avf['size_percent']:+.3f}% |" if op == 'append' else '– | – |'))
        out += ['', '| Frame limit | Parent B | Parent seed pack s | Old archive read = copied B | Old plaintext recompressed | Fault: wrong bytes accepted (fresh / append) | Fault verified bytes at 10/50/90% (append) |',
                '| --- | ---: | ---: | ---: | ---: | --- | --- |']
        for c, item in u['caps'].items():
            p = u['preparation'][c]
            a = item['append']
            fa = item['fault']['append']
            out.append(f"| {cap_name(c)} | {p['parent_bytes']:,} | {p['seed_pack_process_ms'] / 1000:.2f} | {a['old_archive_bytes_read']:,} | "
                       f"{a['old_plaintext_bytes_recompressed']} | {item['fault']['fresh']['wrong_bytes_accepted']} / {fa['wrong_bytes_accepted']} | "
                       + ' / '.join(f"{x['verified_bytes']:,}" for x in fa['cases']) + f" of {fa['input_bytes']:,} |")
        out += ['', 'Read/decode samples: ' + ', '.join(f"`{s['path']}`" for s in u['access_samples']), '']
    Path(sys.argv[2]).write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
