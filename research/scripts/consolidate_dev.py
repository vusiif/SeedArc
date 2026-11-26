# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-030 consolidation cost (research/ideas/append-drift-r0.md §6).

For one recipe, rebuild the DEV-027 append chain (initial 40%, then 15% per batch). After every
append, consolidate-check measures the one-generation rebuild against a fresh recipe-pack of the
same tree (sizes must agree); at the last generation consolidate writes it and the result must equal
the fresh pack byte for byte. Usable on its own: consolidate_dev.py TOOL DATA_DIR MANIFEST OUT_DIR.
"""
import json
import os
from pathlib import Path
import sys

from bounded_expand_dev import load, remove_owned, sha, write_json
from codec_curve_dev import Runner, batches, link

RECIPES = ['solid/zstd-9/g1-r0/frame-32MiB', 'solid/zstd-3/g1-r0/frame-32MiB',
           'solid/lzma2-9/g1-r0/frame-32MiB']


def run_chain(measure, data, groups, rec, chain, cls):
    chain.mkdir()
    cumulative = chain / 'cumulative'
    cumulative.mkdir()
    previous, out = None, None
    for gen, paths in enumerate(groups):
        meta = dict(cls=cls, recipe=rec, generation=gen)
        tag = f'{cls}-{rec.split("/")[1]}-g{gen}'
        batch = chain / f'batch-{gen}'
        link(data, paths, batch)
        for p in paths:
            dest = cumulative / p
            dest.parent.mkdir(parents=True, exist_ok=True)
            os.link(data / p, dest)
        grown = chain / f'gen-{gen}.sadx'
        if previous is None:
            measure(['recipe-pack', 'seed', rec, batch, grown], tag + '-pack', dict(meta, kind='pack'), warm=[batch])
        else:
            res = measure(['recipe-append', previous, batch, grown], tag + '-append', dict(meta, kind='append'),
                          warm=[batch])
            if not res['old_is_prefix']:
                raise AssertionError('append rewrote old data')
            previous.unlink()
        previous = grown
        remove_owned(batch, chain)
        described = measure(['describe', grown], tag + '-describe', dict(meta, kind='describe'))
        if described['consolidation']['generation_count'] != gen + 1:
            raise AssertionError('describe generation count wrong')
        if gen == 0:
            continue
        checked = measure(['consolidate-check', grown], tag + '-check', dict(meta, kind='check'), warm=[grown])
        fresh = chain / 'fresh.sadx'
        fres = measure(['recipe-pack', 'seed', rec, cumulative, fresh], tag + '-fresh', dict(meta, kind='fresh'),
                       warm=[cumulative])
        if checked['archive_bytes_after'] != fres['container_bytes'] or checked['written']:
            raise AssertionError('consolidate-check size differs from a fresh pack')
        if gen == len(groups) - 1:
            one = chain / 'one.sadx'
            done = measure(['consolidate', grown, one], tag + '-consolidate', dict(meta, kind='consolidate'),
                           warm=[grown])
            if sha(one) != sha(fresh) or not done['written']:
                raise AssertionError('consolidated archive differs from a fresh pack')
            verified = measure(['verify', one, cumulative], tag + '-verify', dict(meta, kind='verify'))
            if not verified['byte_exact']:
                raise AssertionError('consolidated archive verify failed')
            out = dict(cls=cls, recipe=rec, sha256=sha(one), bytes=one.stat().st_size)
            one.unlink()
        fresh.unlink()
    previous.unlink()
    remove_owned(chain, chain.parent)
    return out


def run_class(tool, data, manifest, cls, work, results, task_root, recipes=RECIPES, pin_cpu=True, **runner):
    groups, shares = batches(load(data, manifest))
    write_json(results / f'batches-{cls}.json', dict(files=[len(g) for g in groups], cumulative_shares=shares))
    measure = Runner(tool, results / cls, task_root, pin_cpu, **runner)
    done = [run_chain(measure, data, groups, rec, work / f'chain-{cls}', cls) for rec in recipes]
    return dict(commands=len(measure.records), consolidated=done)


def analyse(results):
    """Per class / recipe / generation: archive bytes, saving, consolidate-check cost vs a fresh pack."""
    out = []
    for raw in sorted(Path(results).glob('*/raw.jsonl')):
        items = [json.loads(line) for line in raw.read_text(encoding='utf-8').splitlines()]
        by = {}
        for i in items:
            m = i['meta']
            by.setdefault((m['cls'], m['recipe'], m['generation']), {})[m['kind']] = i
        for (cls, rec, gen), kinds in sorted(by.items()):
            if 'check' not in kinds:
                continue
            c, f = kinds['check']['result'], kinds['fresh']['result']
            row = dict(cls=cls, recipe=rec, generation=gen, archive_bytes=c['archive_bytes_before'],
                       consolidated_bytes=c['archive_bytes_after'], saved_bytes=c['saved_bytes'],
                       saved_share=c['saved_share'], appended_raw_share=kinds['describe']['result']['consolidation']['appended_raw_share'],
                       check_wall_ms=c['call_wall_ms'], check_decode_ms=c['decode_wall_ms'],
                       check_pack_ms=c['pack_wall_ms'], check_peak=c['peak_memory_bytes'],
                       fresh_pack_wall_ms=f['call_wall_ms'], fresh_peak=f['peak_memory_bytes'])
            if 'consolidate' in kinds:
                row['consolidate_wall_ms'] = kinds['consolidate']['result']['call_wall_ms']
            out.append(row)
    return out


if __name__ == '__main__':
    tool, data, manifest, out = map(Path, sys.argv[1:5])
    out.mkdir()
    work = out / 'work'
    work.mkdir()
    done = run_class(tool, data, manifest, 'local', work, out, out, pin_cpu=False, disk_gate=1 << 50)
    remove_owned(work, out)
    rows = analyse(out)
    write_json(out / 'analysis.json', dict(run=done, rows=rows))
    print(json.dumps(rows, indent=1))
