# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-024 measurement: access-profile vs indexed extract-profile on fixed archives.

Per class: pack each frame limit once (SHA must equal the registered reference), verify, then
four rounds with rotated cap order; in each round both profiles run per cap, the order of the
two profiles alternating by round. Every profile reads the archive once beforehand (warm cache).
"""
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

from bounded_expand_dev import disk_bytes, load, sha, write_json

CAPS = [1024**3, 32 * 1024**2, 64 * 1024**2, 128 * 1024**2]
DISK_GATE = 1900 * 1024**2


def run_class(tool, data, manifest, cls, reference, work, results, task_root, caps=None, samples=12,
              pin_cpu=True, disk_gate=DISK_GATE):
    tool, data = Path(tool).resolve(strict=True), Path(data).resolve(strict=True)
    rows = load(data, manifest)
    caps = list(caps or CAPS)
    work.mkdir()
    results.mkdir()
    logs = results / 'commands'
    logs.mkdir()
    affinity = {min(os.sched_getaffinity(0))} if pin_cpu and hasattr(os, 'sched_getaffinity') else None
    orders = [caps[i:] + caps[:i] for i in range(len(caps))]
    write_json(results / 'config.json', dict(cls=cls, manifest_sha256=sha(manifest), tool_sha256=sha(tool),
        runner_sha256=sha(Path(__file__)), files=len(rows), bytes=sum(r['size'] for r in rows),
        frame_limits=caps, orders=orders, samples=samples, affinity=sorted(affinity) if affinity else None,
        profile_order='access then extract on even rounds; reverse on odd rounds',
        cache='archive read once before each profile; no global cache dropping'))
    records = []

    def call(args, label, meta, warm=None):
        if warm is not None:
            with open(warm, 'rb') as stream:
                while stream.read(1024**2):
                    pass
        used = disk_bytes(task_root)
        if used + 64 * 1024**2 >= disk_gate:
            raise RuntimeError('retain output headroom')

        def fixed_cpu():
            os.sched_setaffinity(0, affinity)
        start = time.perf_counter()
        proc = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True, encoding='utf-8',
                              timeout=1800, preexec_fn=fixed_cpu if affinity else None)
        wall = (time.perf_counter() - start) * 1000
        stem = f'{len(records):03d}-{label}'
        (logs / (stem + '.stdout')).write_text(proc.stdout, encoding='utf-8')
        (logs / (stem + '.stderr')).write_text(proc.stderr, encoding='utf-8')
        item = dict(index=len(records), label=label, argv=list(map(str, args)), meta=meta,
                    returncode=proc.returncode, process_wall_ms=wall, disk_before=used, disk_after=disk_bytes(task_root))
        if proc.returncode == 0:
            item['result'] = json.loads(proc.stdout)
        records.append(item)
        with (results / 'raw.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(item, ensure_ascii=False) + '\n')
        print(json.dumps({k: item[k] for k in ['index', 'label', 'returncode', 'process_wall_ms']}), flush=True)
        if proc.returncode or item['disk_after'] >= disk_gate:
            raise RuntimeError(label + ': command or disk gate failed; preserve evidence')
        return item['result']

    archives = {}
    for cap in caps:
        path = work / f'{cap}.sadx'
        packed = call(['bounded-pack', 'seed', cap, data, path], f'pack-{cap}', dict(kind='pack', cap=cap))
        checked = call(['verify', path, data], f'pack-{cap}-verify', dict(kind='verify', cap=cap))
        ref = reference[str(cap)]
        if (not checked['byte_exact'] or path.stat().st_size != packed['container_bytes']
                or sha(path) != ref['sha256'] or path.stat().st_size != ref['bytes']):
            raise AssertionError(f'{cls} {cap}: archive differs from the registered reference')
        archives[cap] = path
    write_json(results / 'archives.json', {str(c): dict(sha256=sha(p), bytes=p.stat().st_size) for c, p in archives.items()})
    for r, order in enumerate(orders):
        for position, cap in enumerate(order):
            kinds = ['access', 'extract'] if r % 2 == 0 else ['extract', 'access']
            got = {}
            for kind_position, kind in enumerate(kinds):
                meta = dict(kind=kind, round=r, position=position, kind_position=kind_position, cap=cap)
                if kind == 'access':
                    args = ['access-profile', archives[cap], data, samples]
                else:
                    out = work / f'out-{r}-{cap}'
                    out.mkdir()
                    args = ['extract-profile', archives[cap], data, samples, out]
                got[kind] = call(args, f'round-{r}-{cap}-{kind}', meta, warm=archives[cap])
                if kind == 'extract':
                    # Windows can list a removed file until another handle (e.g. a scanner)
                    # closes; wait briefly, record it, and fail on anything that stays.
                    waited = time.perf_counter()
                    while any(out.iterdir()) and time.perf_counter() - waited < 5:
                        time.sleep(0.05)
                    records[-1]['cleanup_wait_ms'] = (time.perf_counter() - waited) * 1000
                    if any(out.iterdir()):
                        raise AssertionError('extract profile left output')
                    out.rmdir()
            a, e = got['access'], got['extract']
            size = archives[cap].stat().st_size
            if (not a['byte_exact'] or not e['byte_exact']
                    or [s['path'] for s in a['samples']] != [s['path'] for s in e['samples']]
                    or any(s['decoded_bytes'] > cap or s['decoded_bytes'] < s['requested_bytes']
                           or s['archive_bytes_read'] > size for s in e['samples'])
                    or e['outputs_written'] != e['sample_count'] or e['outputs_removed'] != e['sample_count']):
                raise AssertionError(f'{cls} round {r} cap {cap}: profile check failed')
    for cap, path in archives.items():
        if sha(path) != reference[str(cap)]['sha256']:
            raise AssertionError('archive changed during the profiles')
        path.unlink()
    if len(records) != 2 * len(caps) + 2 * len(caps) * len(caps):
        raise AssertionError('incomplete pre-registered matrix')
    complete = dict(cls=cls, commands=len(records), profiles=2 * len(caps) ** 2, all_byte_exact=True,
                    archives_match_reference=True, same_samples=True)
    write_json(results / 'complete.json', complete)
    return complete


def spread(values):
    return dict(min=min(values), median=statistics.median(values), max=max(values))


def analyse(results):
    """Independent re-check plus summary from raw.jsonl only."""
    config = json.loads((results / 'config.json').read_text(encoding='utf-8'))
    raw = [json.loads(l) for l in (results / 'raw.jsonl').read_text(encoding='utf-8').splitlines()]
    archives = json.loads((results / 'archives.json').read_text(encoding='utf-8'))
    caps, n = config['frame_limits'], len(config['frame_limits'])
    problems = []
    if len(raw) != 2 * n + 2 * n * n or any(r['returncode'] for r in raw):
        problems.append('command count or failure')
    prof = {(r['meta']['round'], r['meta']['cap'], r['meta']['kind']): r for r in raw if r['meta']['kind'] in ('access', 'extract')}
    for cap in caps:
        if sorted(prof[(r, cap, 'access')]['meta']['position'] for r in range(n)) != list(range(n)):
            problems.append(f'cap {cap} positions')
    for (r, cap, kind), item in prof.items():
        first = 'access' if r % 2 == 0 else 'extract'
        if (item['meta']['kind_position'] == 0) != (kind == first):
            problems.append('profile order ' + item['label'])
        if not item['result']['byte_exact']:
            problems.append('not byte exact ' + item['label'])
    paths = {tuple(s['path'] for s in v['result']['samples']) for v in prof.values()}
    if len(paths) != 1:
        problems.append('sample sets differ')
    out = dict(cls=config['cls'], ok=not problems, problems=problems, files=config['files'], bytes=config['bytes'], caps={})
    whole = caps[0]
    for cap in caps:
        size = archives[str(cap)]['bytes']
        ex = [prof[(r, cap, 'extract')]['result'] for r in range(n)]
        ac = [prof[(r, cap, 'access')]['result'] for r in range(n)]
        smp = ex[0]['samples']
        part = lambda key: spread([statistics.median(s[key] for s in e['samples']) for e in ex])
        out['caps'][str(cap)] = dict(archive_bytes=size, sha256=archives[str(cap)]['sha256'],
            extract_median_ms=spread([e['median_ms'] for e in ex]), extract_p95_ms=spread([e['p95_ms'] for e in ex]),
            access_median_ms=spread([a['median_ms'] for a in ac]), access_p95_ms=spread([a['p95_ms'] for a in ac]),
            same_round_access_over_extract=spread([ac[r]['median_ms'] / ex[r]['median_ms'] for r in range(n)]),
            same_round_extract_speedup_vs_whole=spread([prof[(r, whole, 'extract')]['result']['median_ms'] / ex[r]['median_ms'] for r in range(n)]),
            extract_parts_median_ms=dict(index=part('index_ms'), read_frame=part('read_frame_ms'),
                                         decode=part('decode_ms'), write_commit=part('write_commit_ms')),
            archive_bytes_read_per_sample=dict(median=statistics.median(s['archive_bytes_read'] for s in smp),
                                               max=max(s['archive_bytes_read'] for s in smp)),
            index_bytes_read=smp[0]['index_bytes_read'],
            decoded_bytes_per_sample=dict(median=statistics.median(s['decoded_bytes'] for s in smp),
                                          max=max(s['decoded_bytes'] for s in smp)),
            access_decoded_bytes_per_sample=statistics.median(s['decoded_bytes'] for s in ac[0]['samples']),
            extract_peak_memory_bytes=max(e['peak_memory_bytes'] for e in ex),
            access_peak_memory_bytes=max(a['peak_memory_bytes'] for a in ac))
    out['samples'] = [dict(path=s['path'], requested_bytes=s['requested_bytes']) for s in smp]
    return out


if __name__ == '__main__':
    root, target = Path(sys.argv[1]), Path(sys.argv[2])
    report = dict(schema='seedarc.dev024.analysis.r0', classes=[analyse(root / c) for c in sys.argv[3:]])
    report['all_ok'] = all(c['ok'] for c in report['classes'])
    report['limitations'] = ['warm cache, archive pre-read before every profile', 'no fsync', 'one machine, one CPU',
                             'existing public dev, not a holdout', 'four rounds, no statistical significance']
    target.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(dict(all_ok=report['all_ok'], classes=[(c['cls'], c['ok'], c['problems']) for c in report['classes']])))
    sys.exit(0 if report['all_ok'] else 1)
