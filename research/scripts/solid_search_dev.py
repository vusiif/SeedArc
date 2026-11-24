# SPDX-License-Identifier: GPL-3.0-or-later
"""One fixed-host dev observation: shared solid search and independently retained histories."""
import json
import os
from pathlib import Path
import subprocess
import time
from append_history_r2 import sha, prefix_matches, write_json
from append_large_dev import rows, remove_owned, disk_bytes


def run(tool, data, manifest, work, results, task_root):
    selected = rows(data, manifest)
    work.mkdir()
    results.mkdir()
    commands = results/'commands'
    commands.mkdir()
    affinity = {min(os.sched_getaffinity(0))}
    records, artifacts, initial = [], [], {}
    total = sum(r['size'] for r in selected)
    ninitial = sum(r['size'] for r in selected if r['batch']=='initial')
    write_json(results/'config.json', dict(tool_sha256=sha(tool), manifest_sha256=sha(manifest),
        runner_sha256=sha(Path(__file__)), bytes=total, initial_bytes=ninitial,
        files=len(selected), budget_multiple=4, prng_seed=3, policy='near', affinity=sorted(affinity),
        repetition=1, scope='dev only; three append cohorts merged into one update; no holdout',
        cache='each command warms only its input tree; no system cache dropping',
        disk_limit_bytes=2*1024**3, source_root_change='removed artificial linux/; contents and cohorts unchanged'))

    def call(args, label, tree):
        for p in sorted(tree.rglob('*')):
            if p.is_file():
                with p.open('rb') as stream:
                    while stream.read(1024**2):
                        pass
        used = disk_bytes(task_root)
        if used > 1400*1024**2:
            raise RuntimeError('retain conservative output headroom')
        def fixed_cpu():
            os.sched_setaffinity(0, affinity)
        start = time.perf_counter()
        proc = subprocess.run([str(tool), *map(str,args)], capture_output=True, text=True,
                              timeout=1800, preexec_fn=fixed_cpu)
        stem = f'{len(records):03d}-{label}'
        (commands/(stem+'.stdout')).write_text(proc.stdout)
        (commands/(stem+'.stderr')).write_text(proc.stderr)
        item = dict(index=len(records), label=label, argv=list(map(str,args)), returncode=proc.returncode,
                    process_wall_ms=(time.perf_counter()-start)*1000, disk_before=used,
                    disk_after=disk_bytes(task_root))
        if proc.returncode == 0:
            item['result'] = json.loads(proc.stdout)
        records.append(item)
        with (results/'raw.jsonl').open('a') as stream:
            stream.write(json.dumps(item)+'\n')
        print(json.dumps({k:item[k] for k in ['index','label','returncode','process_wall_ms']}), flush=True)
        if proc.returncode or item['disk_after'] > 1900*1024**2:
            raise RuntimeError('command or disk gate failed; retain evidence')
        return item['result']

    first, added = work/'initial', work/'added'
    first.mkdir()
    added.mkdir()
    # One additional directory name per input inode; monitor counts hardlink names.
    for row in selected:
        dest = (first if row['batch']=='initial' else added)/row['path']
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.link(data/row['path'], dest)

    def checked(path, result, label, tree, generations):
        verification = call(['verify',path,tree],label+'-verify',tree)
        if not verification.get('byte_exact') or verification['generations'] != generations:
            raise AssertionError('roundtrip/generation mismatch')
        if path.stat().st_size != result['container_bytes']:
            raise AssertionError('physical size disagrees')
        obj = dict(label=label, bytes=path.stat().st_size, sha256=sha(path), result=result)
        artifacts.append(obj)
        write_json(results/'archives.json',artifacts)
        return obj

    def search(mode, start, tree, path, nbytes, label):
        result = call(['solid-search',mode,start,tree,path,4*nbytes,3,'near'],label,tree)
        if (result['evaluations'] != 4 or result['budget_spent_bytes'] != 4*nbytes
            or result['best_bytes'] > min(result['start_bytes'],result['baseline_bytes'])
            or not result['byte_exact']):
            raise AssertionError('search budget/fallback failure')
        return result

    def compare(pair, label):
        result = call(['payload-compare',pair['seed'],pair['explicit']],label,data)
        if not result['equal']:
            raise AssertionError('representation changed payload')

    paths = {}
    for mode in ['seed','explicit']:
        path = work/(mode+'-initial.sadx')
        result = search(mode,'default',first,path,ninitial,'initial-'+mode)
        initial[mode] = checked(path,result,'initial-'+mode,first,1)
        paths[mode] = path
    if initial['seed']['result']['evaluated'] != initial['explicit']['result']['evaluated']:
        raise AssertionError('initial candidate scores differ')
    compare(paths,'initial-payload')
    # Delete initial input names now; parents are retained for the reuse branch.
    remove_owned(first,work)
    for variant in ['fixed','reuse','warm','cold']:
        pair, scores = {}, {}
        for mode in ['seed','explicit']:
            path = work/f'{variant}-{mode}.sadx'
            label = variant+'-'+mode
            if variant == 'fixed':
                result = call(['pack',mode+'-solid','0x0' if mode=='seed' else '0:1',data,path,'off'],label,data)
            elif variant == 'reuse':
                result = call(['append',paths[mode],added,path,'segment'],label,added)
                if (not result.get('old_is_prefix') or not prefix_matches(paths[mode],path)
                    or sha(paths[mode]) != initial[mode]['sha256']):
                    raise AssertionError('parent prefix changed')
            else:
                previous = initial[mode]['result']
                p = previous['best_plan']
                start = 'default' if variant=='cold' else (hex(previous['strategy_seed']) if mode=='seed'
                    else f"{p['rule']}:{p['groups']}")
                result = search(mode,start,data,path,total,label)
            checked(path,result,label,data,2 if variant=='reuse' else 1)
            pair[mode], scores[mode] = path, result
        if variant in ['warm','cold'] and scores['seed']['evaluated'] != scores['explicit']['evaluated']:
            raise AssertionError('same-start representation candidate scores differ')
        compare(pair,variant+'-payload')
        for path in pair.values():
            path.unlink()
    if len(records)!=25 or len(artifacts)!=10:
        raise AssertionError('incomplete matrix')
    write_json(results/'complete.json',dict(commands=25,archives_verified=10,payload_compares=5,
        search_commands=6,all_byte_exact=True))
    for path in paths.values():
        path.unlink()
    remove_owned(added,work)
