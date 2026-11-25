# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-023 independent audit and analysis of bounded_expand_dev unit results.

usage: bounded_expand_report.py RESULTS_ROOT OUT.json [UNIT ...]
RESULTS_ROOT holds one directory per unit named <class>-<batch>, each with config.json,
raw.jsonl, archives.json, parents.json and complete.json. The audit re-derives every
pre-registered invariant from the raw records instead of trusting complete.json.
"""
import json
from pathlib import Path
import statistics
import sys

UNITS = ['source-added10', 'source-added40', 'text-added10', 'text-added40']
REFERENCE = 1024**3


def load_unit(root):
    config = json.loads((root / 'config.json').read_text(encoding='utf-8'))
    raw = [json.loads(l) for l in (root / 'raw.jsonl').read_text(encoding='utf-8').splitlines()]
    archives = json.loads((root / 'archives.json').read_text(encoding='utf-8'))
    parents = json.loads((root / 'parents.json').read_text(encoding='utf-8'))
    return config, raw, archives, parents


def audit(root):
    config, raw, archives, parents = load_unit(root)
    caps, rounds = config['frame_limits'], config['rounds']
    n = len(caps)
    problems = []
    if len(raw) != 5 * n + 6 * n * rounds + 2 * n:
        problems.append(f'command count {len(raw)}')
    if [r['index'] for r in raw] != list(range(len(raw))) or any(r['returncode'] for r in raw):
        problems.append('index gap or failed command')
    verifies = [r for r in raw if r['meta'].get('kind') == 'verify']
    if len(verifies) != len(archives) or not all(r['result']['byte_exact'] for r in verifies):
        problems.append('verify count / byte exactness')
    encodes = [r for r in raw if r['meta']['phase'] == 'repeat' and r['meta']['kind'] == 'encode']
    if len(encodes) != 2 * n * rounds:
        problems.append('repeat encode count')
    for cap in caps:
        positions = sorted(r['meta']['position'] for r in encodes if r['meta']['cap'] == cap and r['meta']['operation'] == 'fresh')
        if positions != list(range(rounds)):
            problems.append(f'cap {cap} positions {positions}')
    for r in encodes:
        first = 'fresh' if r['meta']['round'] % 2 == 0 else 'append'
        if (r['meta']['operation_position'] == 0) != (r['meta']['operation'] == first):
            problems.append('operation order ' + r['label'])
    by_label = {a['label']: a for a in archives}
    shas = {}
    for r in encodes:
        a = by_label[r['label']]
        if a['bytes'] != r['result']['container_bytes'] or r['result']['max_frame_raw_bytes'] > r['meta']['cap']:
            problems.append('size / cap ' + r['label'])
        shas.setdefault((r['meta']['cap'], r['meta']['operation']), set()).add(a['sha256'])
        if r['meta']['operation'] == 'append':
            p = parents[str(r['meta']['cap'])]
            res = r['result']
            if (not res['old_is_prefix'] or res['old_plaintext_bytes_recompressed'] != 0
                    or res['old_archive_bytes_read'] != p['bytes'] or res['old_archive_bytes_copied'] != p['bytes']):
                problems.append('append accounting ' + r['label'])
    if any(len(s) != 1 for s in shas.values()):
        problems.append('repeat SHA not stable')
    for cap in caps:
        seed = by_label[f'prepare-{cap}-seed']
        if seed['sha256'] != parents[str(cap)]['sha256']:
            problems.append(f'parent record {cap}')
    compares = [r for r in raw if r['meta']['kind'] == 'payload-compare']
    if len(compares) != n or not all(r['result']['equal'] for r in compares):
        problems.append('seed / explicit payload compare')
    accesses = [r for r in raw if r['meta']['kind'] == 'access']
    sample_sets = {tuple(s['path'] for s in r['result']['samples']) for r in accesses}
    if len(accesses) != 2 * n * rounds or len(sample_sets) != 1 or not all(r['result']['byte_exact'] for r in accesses):
        problems.append('access count / sample set / exactness')
    faults = [r for r in raw if r['meta']['kind'] == 'fault']
    if len(faults) != 2 * n or any(r['result']['wrong_bytes_accepted'] or r['result']['outputs_written'] for r in faults):
        problems.append('fault profiles')
    return dict(unit=root.name, ok=not problems, problems=problems, commands=len(raw), archives=len(archives),
                sha_by_cap_operation={f'{c}-{o}': sorted(s)[0] for (c, o), s in shas.items()},
                sample_paths=sorted(sample_sets)[0] if sample_sets else [])


def spread(values):
    return dict(min=min(values), median=statistics.median(values), max=max(values))


def analyse(root):
    config, raw, archives, parents = load_unit(root)
    caps, rounds = config['frame_limits'], config['rounds']
    by_label = {a['label']: a for a in archives}
    rows = {(r['meta']['round'], r['meta']['cap'], r['meta']['operation'], r['meta']['kind']): r
            for r in raw if r['meta']['phase'] == 'repeat'}
    prep = {}
    for cap in caps:
        enc = [r for r in raw if r['meta']['phase'] == 'prepare' and r['meta']['cap'] == cap]
        prep[str(cap)] = dict(parent_bytes=parents[str(cap)]['bytes'],
            seed_pack_process_ms=next(r['process_wall_ms'] for r in enc if r['label'] == f'prepare-{cap}-seed'),
            explicit_pack_process_ms=next(r['process_wall_ms'] for r in enc if r['label'] == f'prepare-{cap}-explicit'),
            verify_process_ms=sum(r['process_wall_ms'] for r in enc if r['meta']['kind'] == 'verify'))
    out = dict(unit=root.name, cls=config['cls'], batch=config['batch'], files=config['files'], bytes=config['bytes'],
               added_bytes=config['added_bytes'], added_ratio=config['added_ratio'], preparation=prep, caps={})
    for cap in caps:
        item = {}
        for op in ['fresh', 'append']:
            enc = [rows[(r, cap, op, 'encode')] for r in range(rounds)]
            acc = [rows[(r, cap, op, 'access')]['result'] for r in range(rounds)]
            ver = [rows[(r, cap, op, 'verify')] for r in range(rounds)]
            res = enc[0]['result']
            size = by_label[enc[0]['label']]['bytes']
            ref = by_label[rows[(0, REFERENCE, op, 'encode')]['label']]['bytes'] if REFERENCE in caps else None
            entry = dict(container_bytes=size, sha256=by_label[enc[0]['label']]['sha256'],
                vs_reference_percent=None if ref is None else (size / ref - 1) * 100,
                frame_count=res['frame_count'], max_frame_raw_bytes=res['max_frame_raw_bytes'],
                process_wall_ms=spread([e['process_wall_ms'] for e in enc]),
                call_wall_ms=spread([e['result']['call_wall_ms'] for e in enc]) if 'call_wall_ms' in res else None,
                verify_process_ms=spread([v['process_wall_ms'] for v in ver]),
                access_median_ms=spread([a['median_ms'] for a in acc]), access_p95_ms=spread([a['p95_ms'] for a in acc]),
                access_decoded_bytes_total=acc[0]['decoded_bytes_total'],
                access_requested_bytes_total=acc[0]['requested_bytes_total'],
                access_peak_memory_bytes=max(a['peak_memory_bytes'] for a in acc),
                encode_peak_memory_bytes=max(e['result']['peak_memory_bytes'] for e in enc))
            if REFERENCE in caps:
                pair = []
                for r in range(rounds):
                    base = rows[(r, REFERENCE, op, 'access')]['result']['median_ms']
                    pair.append(base / rows[(r, cap, op, 'access')]['result']['median_ms'])
                entry['same_round_access_speedup_vs_reference'] = spread(pair)
                entry['same_round_encode_ratio_vs_reference'] = spread(
                    [rows[(r, cap, op, 'encode')]['process_wall_ms'] / rows[(r, REFERENCE, op, 'encode')]['process_wall_ms']
                     for r in range(rounds)])
            if op == 'append':
                entry.update(old_archive_bytes_read=res['old_archive_bytes_read'],
                             old_archive_bytes_copied=res['old_archive_bytes_copied'],
                             old_plaintext_bytes_recompressed=res['old_plaintext_bytes_recompressed'],
                             archive_bytes_written=res['archive_bytes_written'])
            item[op] = entry
        fresh, app = item['fresh'], item['append']
        item['append_vs_fresh'] = dict(size_percent=(app['container_bytes'] / fresh['container_bytes'] - 1) * 100,
            same_round_wall_ratio=spread([rows[(r, cap, 'append', 'encode')]['process_wall_ms'] /
                                          rows[(r, cap, 'fresh', 'encode')]['process_wall_ms'] for r in range(rounds)]))
        item['fault'] = {op: next(r['result'] for r in raw if r['meta']['kind'] == 'fault' and r['meta']['cap'] == cap
                                  and r['meta']['operation'] == op) for op in ['fresh', 'append']}
        out['caps'][str(cap)] = item
    sample = rows[(0, caps[0], 'fresh', 'access')]['result']['samples']
    out['access_samples'] = [dict(path=s['path'], requested_bytes=s['requested_bytes']) for s in sample]
    return out


def main():
    root, target = Path(sys.argv[1]), Path(sys.argv[2])
    units = sys.argv[3:] or [u for u in UNITS if (root / u / 'complete.json').exists()]
    audits = [audit(root / u) for u in units]
    report = dict(schema='seedarc.dev023.analysis.r1', units=units, audits=audits,
                  all_audits_ok=all(a['ok'] for a in audits))
    # The two batches of one class share the final tree, so fresh archives must be identical.
    cross = {}
    for cls in ['source', 'text']:
        pair = [a for a in audits if a['unit'].startswith(cls + '-')]
        if len(pair) == 2:
            f = [{k: v for k, v in a['sha_by_cap_operation'].items() if k.endswith('-fresh')} for a in pair]
            cross[cls] = dict(fresh_identical_across_batches=f[0] == f[1],
                              same_access_samples=pair[0]['sample_paths'] == pair[1]['sample_paths'])
            report['all_audits_ok'] &= f[0] == f[1] and pair[0]['sample_paths'] == pair[1]['sample_paths']
    report['cross_batch'] = cross
    report['analysis'] = [analyse(root / u) for u in units] if report['all_audits_ok'] else None
    report['limitations'] = ['existing public dev, not a holdout', 'warm cache, one machine, one CPU per measured command',
        'read/decode diagnostic, no file written to disk', 'append copies the whole old archive',
        'fault diagnostic assumes intact header and index', 'four rounds give no statistical significance',
        'text class 77.8 MiB < 128 MiB: 128 MiB and 1 GiB may share one layout']
    Path(target).write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(dict(all_audits_ok=report['all_audits_ok'], units=[(a['unit'], a['ok'], a['problems']) for a in audits],
                          cross=cross)))
    return 0 if report['all_audits_ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
