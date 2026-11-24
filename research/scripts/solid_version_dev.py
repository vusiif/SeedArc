# SPDX-License-Identifier: GPL-3.0-or-later
"""Full candidate reference on v0, then same-budget plan reuse on a real v1 projection."""
import json
import os
from pathlib import Path
import subprocess
import time
from append_history_r2 import sha, write_json
from append_large_dev import rows, disk_bytes
from solid_search_report import audit_search


def run(tool, v0, v1, m0, m1, work, results, task_root):
    selected0,selected1=rows(v0,m0),rows(v1,m1)
    work.mkdir()
    results.mkdir()
    commands=results/'commands'
    commands.mkdir()
    affinity={min(os.sched_getaffinity(0))} if hasattr(os,'sched_getaffinity') else None
    records,artifacts,initial=[],[],{}
    totals=[sum(r['size'] for r in rs) for rs in [selected0,selected1]]
    write_json(results/'config.json',dict(tool_sha256=sha(tool),runner_sha256=sha(Path(__file__)),
        v0_manifest_sha256=sha(m0),v1_manifest_sha256=sha(m1),bytes=totals,files=len(selected0),
        initial_budget_multiple=29,update_budget_multiple=8,prng_seed=3,
        initial_policy='enumerate',update_policy='near',affinity=sorted(affinity) if affinity else None,
        repetitions=1,scope='mixed public dev; fixed-path real Linux6.12 to6.12.5 modifications',
        update='full snapshot re-encoding with inherited concrete plan; not archive append',
        cache='warm each command input tree; no global cache drop'))

    def call(args,label,tree):
        for p in sorted(tree.rglob('*')):
            if p.is_file():
                with p.open('rb') as stream:
                    while stream.read(1024**2):
                        pass
        used=disk_bytes(task_root)
        reserve=max(totals)+len(selected0)*1024+16*1024**2
        if used+reserve>=1900*1024**2:
            raise RuntimeError('retain conservative output headroom')
        def fixed_cpu():
            os.sched_setaffinity(0,affinity)
        start=time.perf_counter()
        proc=subprocess.run([str(tool),*map(str,args)],capture_output=True,text=True,
            encoding='utf-8',timeout=3600,preexec_fn=fixed_cpu if affinity else None)
        stem=f'{len(records):03d}-{label}'
        (commands/(stem+'.stdout')).write_text(proc.stdout,encoding='utf-8')
        (commands/(stem+'.stderr')).write_text(proc.stderr,encoding='utf-8')
        item=dict(index=len(records),label=label,argv=list(map(str,args)),returncode=proc.returncode,
            process_wall_ms=(time.perf_counter()-start)*1000,disk_before=used,disk_after=disk_bytes(task_root))
        if proc.returncode==0:
            item['result']=json.loads(proc.stdout)
        records.append(item)
        with (results/'raw.jsonl').open('a',encoding='utf-8') as stream:
            stream.write(json.dumps(item)+'\n')
        print(json.dumps({k:item[k] for k in ['index','label','returncode','process_wall_ms']}),flush=True)
        if proc.returncode or item['disk_after']>=1900*1024**2:
            raise RuntimeError('command/disk gate failed; preserve evidence')
        return item['result']

    def checked(path,result,label,tree):
        check=call(['verify',path,tree],label+'-verify',tree)
        if not check.get('byte_exact') or check['generations']!=1 or path.stat().st_size!=result['container_bytes']:
            raise AssertionError('snapshot verification or physical size failed')
        obj=dict(label=label,bytes=path.stat().st_size,sha256=sha(path),result=result)
        artifacts.append(obj)
        write_json(results/'archives.json',artifacts)
        return obj

    def plan_arg(mode,result):
        p=result['best_plan']
        return hex(result['strategy_seed']) if mode=='seed' else f"{p['rule']}:{p['groups']}"

    def search(mode,start,tree,path,nbytes,multiple,policy,label):
        result=call(['solid-search',mode,start,tree,path,nbytes*multiple,3,policy],label,tree)
        audit_search(result)
        if result['evaluations']!=multiple or result['budget_spent_bytes']!=nbytes*multiple:
            raise AssertionError('incomplete planned search budget')
        return result

    for variant in ['initial','fixed','reuse','warm','cold']:
        tree=v0 if variant=='initial' else v1
        pair,scores={},{}
        for mode in ['seed','explicit']:
            path=work/f'{variant}-{mode}.sadx'
            label=variant+'-'+mode
            if variant=='initial':
                result=search(mode,'default',tree,path,totals[0],29,'enumerate',label)
            elif variant in ['fixed','reuse']:
                start=('0x0' if mode=='seed' else '0:1') if variant=='fixed' else plan_arg(mode,initial[mode]['result'])
                result=call(['pack',mode+'-solid',start,tree,path,'off'],label,tree)
            else:
                start='default' if variant=='cold' else plan_arg(mode,initial[mode]['result'])
                result=search(mode,start,tree,path,totals[1],8,'near',label)
            item=checked(path,result,label,tree)
            if variant=='initial':
                initial[mode]=item
            pair[mode],scores[mode]=path,result
        if variant in ['initial','warm','cold'] and scores['seed']['evaluated']!=scores['explicit']['evaluated']:
            raise AssertionError('same operator produced representation-specific candidates/scores')
        compared=call(['payload-compare',pair['seed'],pair['explicit']],variant+'-payload',tree)
        if not compared['equal']:
            raise AssertionError('representation changed compressed payload')
        for p in pair.values():
            p.unlink()
    # The real patch must never change the initial snapshot or original dev source.
    rows(v0,m0)
    if len(records)!=25 or len(artifacts)!=10:
        raise AssertionError('incomplete version matrix')
    write_json(results/'complete.json',dict(commands=25,archives_verified=10,payload_compares=5,
        search_commands=6,all_byte_exact=True,v0_unchanged=True))
