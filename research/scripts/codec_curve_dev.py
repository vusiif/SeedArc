# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-026 codec / level curve and DEV-027 append lifecycle (research/configs/codec-curve-dev-r0.json).

One measured command at a time, pinned to one CPU; every archive is verified byte for byte.
"""
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

from bounded_expand_dev import disk_bytes, load, remove_owned, sha, write_json

CODECS = [('zstd', 1), ('zstd', 3), ('zstd', 6), ('zstd', 9), ('zstd', 12), ('zstd', 15), ('zstd', 19),
          ('lzma2', 1), ('lzma2', 3), ('lzma2', 6), ('lzma2', 9)]
LAYOUTS = ['whole', '32MiB', '64MiB']
LIMITS = {'whole': 1 << 30, '16MiB': 16 << 20, '32MiB': 32 << 20, '64MiB': 64 << 20}
LIFECYCLE = ['solid/zstd-9/g1-r0/frame-whole', 'solid/zstd-9/g1-r0/frame-32MiB',
             'solid/zstd-9/g1-r0/frame-64MiB', 'solid/zstd-3/g1-r0/frame-32MiB']
SHARES = [0.40, 0.55, 0.70, 0.85, 1.00]
DISK_GATE = 1900 * 1024**2


def recipe(codec, level, layout):
    return f'solid/{codec}-{level}/g1-r0/frame-{layout}'


class Runner:
    def __init__(self, tool, results, task_root, pin_cpu=True, disk_gate=DISK_GATE, timeout=3600):
        self.tool, self.results, self.task_root = Path(tool), results, task_root
        self.affinity = {min(os.sched_getaffinity(0))} if pin_cpu and hasattr(os, 'sched_getaffinity') else None
        self.disk_gate, self.timeout, self.records = disk_gate, timeout, []
        results.mkdir()
        (results / 'commands').mkdir()

    def __call__(self, args, label, meta, warm=()):
        for tree in warm:
            for p in sorted(Path(tree).rglob('*')):
                if p.is_file():
                    with p.open('rb') as s:
                        while s.read(1024**2):
                            pass
        used = disk_bytes(self.task_root)
        if used + 64 * 1024**2 >= self.disk_gate:
            raise RuntimeError('retain output headroom')

        def fixed_cpu():
            os.sched_setaffinity(0, self.affinity)
        start = time.perf_counter()
        proc = subprocess.run([str(self.tool), *map(str, args)], capture_output=True, text=True, encoding='utf-8',
                              timeout=self.timeout, preexec_fn=fixed_cpu if self.affinity else None)
        wall = (time.perf_counter() - start) * 1000
        stem = f'{len(self.records):04d}-{label}'
        (self.results / 'commands' / (stem + '.stdout')).write_text(proc.stdout, encoding='utf-8')
        (self.results / 'commands' / (stem + '.stderr')).write_text(proc.stderr, encoding='utf-8')
        item = dict(index=len(self.records), label=label, argv=list(map(str, args)), meta=meta,
                    returncode=proc.returncode, process_wall_ms=wall, disk_before=used,
                    disk_after=disk_bytes(self.task_root))
        if proc.returncode == 0 and proc.stdout.strip():
            item['result'] = json.loads(proc.stdout)
        self.records.append(item)
        with (self.results / 'raw.jsonl').open('a', encoding='utf-8') as s:
            s.write(json.dumps(item, ensure_ascii=False) + '\n')
        print(json.dumps({k: item[k] for k in ['index', 'label', 'returncode', 'process_wall_ms']}), flush=True)
        if proc.returncode or item['disk_after'] >= self.disk_gate:
            raise RuntimeError(label + ': command or disk gate failed; preserve evidence')
        return item.get('result')


def empty_dir(path):
    waited = time.perf_counter()
    while any(path.iterdir()) and time.perf_counter() - waited < 5:
        time.sleep(0.05)
    if any(path.iterdir()):
        raise AssertionError('extract profile left output')
    path.rmdir()


def run_curve(tool, data, manifest, cls, work, results, task_root, codecs=None, layouts=None, rounds=2,
              samples=12, pin_cpu=True, disk_gate=DISK_GATE):
    rows = load(data, manifest)
    codecs, layouts = list(codecs or CODECS), list(layouts or LAYOUTS)
    points = [(c, l, lay) for lay in layouts for c, l in codecs]
    orders = [points if r % 2 == 0 else list(reversed(points)) for r in range(rounds)]
    work.mkdir()
    run = Runner(tool, results, task_root, pin_cpu, disk_gate)
    write_json(results / 'config.json', dict(cls=cls, manifest_sha256=sha(manifest), tool_sha256=sha(tool),
        runner_sha256=sha(Path(__file__)), files=len(rows), bytes=sum(r['size'] for r in rows),
        points=[recipe(*p) for p in points], rounds=rounds, samples=samples,
        affinity=sorted(run.affinity) if run.affinity else None))
    hashes = {}
    for r, order in enumerate(orders):
        for position, (codec, level, layout) in enumerate(order):
            rec = recipe(codec, level, layout)
            meta = dict(round=r, position=position, codec=codec, level=level, layout=layout, recipe=rec)
            out = work / 'point.sadx'
            packed = run(['recipe-pack', 'seed', rec, data, out], f'r{r}-{codec}{level}-{layout}-pack',
                         dict(meta, kind='pack'), warm=[data])
            checked = run(['verify', out, data], f'r{r}-{codec}{level}-{layout}-verify', dict(meta, kind='verify'))
            prof = work / 'profile'
            prof.mkdir()
            extracted = run(['extract-profile', out, data, samples, prof], f'r{r}-{codec}{level}-{layout}-extract',
                            dict(meta, kind='extract'), warm=[])
            empty_dir(prof)
            digest = sha(out)
            if (not checked['byte_exact'] or not extracted['byte_exact'] or packed['recipe'] != rec
                    or out.stat().st_size != packed['container_bytes']
                    or packed['max_frame_raw_bytes'] > LIMITS[layout]):
                raise AssertionError(rec + ': point check failed')
            if r == 0:
                described = run(['describe', out], f'r{r}-{codec}{level}-{layout}-describe', dict(meta, kind='describe'))
                if described['generations'][0]['recipe'] != rec:
                    raise AssertionError(rec + ': describe label differs')
            if hashes.setdefault(rec, digest) != digest:
                raise AssertionError(rec + ': archive SHA differs between rounds')
            out.unlink()
    write_json(results / 'archives.json', hashes)
    remove_owned(work, work.parent)
    complete = dict(cls=cls, commands=len(run.records), points=len(points), rounds=rounds, all_byte_exact=True,
                    sha_stable=True)
    write_json(results / 'complete.json', complete)
    return complete


def batches(rows):
    total = sum(r['size'] for r in rows)
    order = sorted(rows, key=lambda r: hashlib.sha256(('dev027-batch:' + r['path']).encode()).hexdigest())
    cuts, cumulative, k = [], 0, 0
    prefix = [0]
    for r in order:
        cumulative += r['size']
        prefix.append(cumulative)
    for share in SHARES[:-1]:
        target = share * total
        best = min(range(max(cuts[-1] + 1 if cuts else 1, 1), len(order)), key=lambda i: (abs(prefix[i] - target), i))
        cuts.append(best)
    cuts.append(len(order))
    out, start = [], 0
    for c in cuts:
        out.append([r['path'] for r in order[start:c]])
        start = c
    return out, [prefix[c] / total for c in cuts]


def link(data, paths, target):
    target.mkdir()
    for p in paths:
        dest = target / p
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.link(data / p, dest)


def run_lifecycle(tool, data, manifest, work, results, task_root, recipes=None, rounds=2, pin_cpu=True,
                  disk_gate=DISK_GATE):
    rows = load(data, manifest)
    recipes = list(recipes or LIFECYCLE)
    groups, shares = batches(rows)
    work.mkdir()
    run = Runner(tool, results, task_root, pin_cpu, disk_gate)
    write_json(results / 'config.json', dict(manifest_sha256=sha(manifest), tool_sha256=sha(tool),
        runner_sha256=sha(Path(__file__)), recipes=recipes, rounds=rounds, batch_files=[len(g) for g in groups],
        cumulative_shares=shares, affinity=sorted(run.affinity) if run.affinity else None))
    hashes = {}
    for r in range(rounds):
        for position, rec in enumerate(recipes if r % 2 == 0 else list(reversed(recipes))):
            cumulative = work / 'cumulative'
            cumulative.mkdir()
            previous = None
            for gen, paths in enumerate(groups):
                batch = work / f'batch-{gen}'
                link(data, paths, batch)
                for p in paths:
                    dest = cumulative / p
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    os.link(data / p, dest)
                meta = dict(round=r, position=position, recipe=rec, generation=gen)
                grown = work / f'gen-{gen}.sadx'
                if previous is None:
                    res = run(['recipe-pack', 'seed', rec, batch, grown], f'r{r}-{position}-g{gen}-pack',
                              dict(meta, kind='append'), warm=[batch])
                else:
                    res = run(['recipe-append', previous, batch, grown], f'r{r}-{position}-g{gen}-append',
                              dict(meta, kind='append'), warm=[batch])
                    if not res['old_is_prefix'] or res['old_plaintext_bytes_recompressed'] != 0:
                        raise AssertionError('append rewrote old data')
                check = run(['verify', grown, cumulative], f'r{r}-{position}-g{gen}-verify', dict(meta, kind='verify'))
                if not check['byte_exact'] or check['generations'] != gen + 1:
                    raise AssertionError('append verify failed')
                fresh = work / 'fresh.sadx'
                fres = run(['recipe-pack', 'seed', rec, cumulative, fresh], f'r{r}-{position}-g{gen}-fresh',
                           dict(meta, kind='fresh'), warm=[cumulative])
                fcheck = run(['verify', fresh, cumulative], f'r{r}-{position}-g{gen}-fresh-verify',
                             dict(meta, kind='fresh-verify'))
                if not fcheck['byte_exact']:
                    raise AssertionError('fresh verify failed')
                for key, path in [(f'{rec}|{gen}|append', grown), (f'{rec}|{gen}|fresh', fresh)]:
                    if hashes.setdefault(key, sha(path)) != sha(path):
                        raise AssertionError(key + ': SHA differs between rounds')
                fresh.unlink()
                if previous is not None:
                    previous.unlink()
                previous = grown
                remove_owned(batch, work)
            previous.unlink()
            remove_owned(cumulative, work)
    write_json(results / 'archives.json', hashes)
    remove_owned(work, work.parent)
    complete = dict(commands=len(run.records), recipes=len(recipes), generations=len(groups), rounds=rounds,
                    all_byte_exact=True, sha_stable=True)
    write_json(results / 'complete.json', complete)
    return complete


def spread(values):
    return dict(min=min(values), median=statistics.median(values), max=max(values))


def analyse_curve(results):
    raw = [json.loads(l) for l in (results / 'raw.jsonl').read_text(encoding='utf-8').splitlines()]
    cfg = json.loads((results / 'config.json').read_text(encoding='utf-8'))
    by = {}
    for item in raw:
        m = item['meta']
        by.setdefault(m['recipe'], {}).setdefault(m['kind'], []).append(item)
    points = []
    for rec in cfg['points']:
        d = by[rec]
        packs, verifies, extracts = d['pack'], d['verify'], d['extract']
        pr = packs[0]['result']
        points.append(dict(recipe=rec, codec=packs[0]['meta']['codec'], level=packs[0]['meta']['level'],
            layout=packs[0]['meta']['layout'], archive_bytes=pr['container_bytes'], frames=pr['frame_count'],
            ratio=cfg['bytes'] / pr['container_bytes'],
            pack_wall_ms=spread([p['process_wall_ms'] for p in packs]),
            pack_cpu_ms=spread([p['result']['pack_cpu_ms'] for p in packs]),
            encode_peak_memory_bytes=max(p['result']['peak_memory_bytes'] for p in packs),
            verify_wall_ms=spread([v['process_wall_ms'] for v in verifies]),
            extract_median_ms=spread([e['result']['median_ms'] for e in extracts]),
            extract_p95_ms=spread([e['result']['p95_ms'] for e in extracts]),
            index_manifest_ms=statistics.median(s['index_manifest_ms'] for e in extracts for s in e['result']['samples']),
            index_read_ms=statistics.median(s['index_read_ms'] for e in extracts for s in e['result']['samples']),
            extract_peak_memory_bytes=max(e['result']['peak_memory_bytes'] for e in extracts)))
    for p in points:
        p['pareto_size_vs_pack'] = not any(
            q is not p and q['archive_bytes'] <= p['archive_bytes'] and
            q['pack_wall_ms']['median'] <= p['pack_wall_ms']['median'] and
            (q['archive_bytes'] < p['archive_bytes'] or q['pack_wall_ms']['median'] < p['pack_wall_ms']['median'])
            for q in points)
    budgets = []
    for ref in [p for p in points if p['codec'] == 'zstd' and p['layout'] == 'whole']:
        within = [p for p in points if p['pack_wall_ms']['median'] <= ref['pack_wall_ms']['median']]
        best = min(within, key=lambda p: (p['archive_bytes'], p['pack_wall_ms']['median']))
        budgets.append(dict(budget_from=ref['recipe'], budget_ms=ref['pack_wall_ms']['median'],
                            smallest=best['recipe'], smallest_bytes=best['archive_bytes'],
                            vs_reference_percent=(best['archive_bytes'] / ref['archive_bytes'] - 1) * 100))
    return dict(cls=cfg['cls'], files=cfg['files'], bytes=cfg['bytes'], points=points, same_time_budget=budgets)


def analyse_lifecycle(results):
    raw = [json.loads(l) for l in (results / 'raw.jsonl').read_text(encoding='utf-8').splitlines()]
    cfg = json.loads((results / 'config.json').read_text(encoding='utf-8'))
    out = []
    for rec in cfg['recipes']:
        gens = []
        for g in range(len(cfg['batch_files'])):
            pick = lambda kind: [i for i in raw if i['meta']['recipe'] == rec and i['meta']['generation'] == g
                                 and i['meta']['kind'] == kind]
            app, fresh = pick('append'), pick('fresh')
            ab, fb = app[0]['result']['container_bytes'], fresh[0]['result']['container_bytes']
            gens.append(dict(generation=g, cumulative_share=cfg['cumulative_shares'][g], append_bytes=ab,
                             fresh_bytes=fb, drift_percent=(ab / fb - 1) * 100,
                             append_wall_ms=spread([a['process_wall_ms'] for a in app]),
                             fresh_wall_ms=spread([f['process_wall_ms'] for f in fresh]),
                             old_bytes_copied=app[0]['result'].get('old_archive_bytes_copied', 0)))
        total_append = sum(g['append_wall_ms']['median'] for g in gens)
        out.append(dict(recipe=rec, generations=gens, append_chain_wall_ms=total_append,
                        old_bytes_copied_total=sum(g['old_bytes_copied'] for g in gens),
                        rebuild_every_step_wall_ms=sum(g['fresh_wall_ms']['median'] for g in gens)))
    return dict(batch_files=cfg['batch_files'], cumulative_shares=cfg['cumulative_shares'], recipes=out)


if __name__ == '__main__':
    root, target = Path(sys.argv[1]), Path(sys.argv[2])
    report = dict(schema='seedarc.dev026-027.analysis.r0',
                  curve=[analyse_curve(root / c) for c in ['source', 'text'] if (root / c / 'complete.json').exists()],
                  lifecycle=analyse_lifecycle(root / 'lifecycle') if (root / 'lifecycle' / 'complete.json').exists() else None,
                  limitations=['warm cache, single pinned CPU, no multithreading', 'one machine, two rounds',
                               'existing public dev, not a holdout', 'extract has no fsync'])
    target.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(dict(curve=[c['cls'] for c in report['curve']], lifecycle=report['lifecycle'] is not None)))
