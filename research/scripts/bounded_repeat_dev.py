# SPDX-License-Identifier: GPL-3.0-or-later
"""Balanced cap positions; repeat fresh pack and append from immutable generation3 parents."""
import json
import os
from pathlib import Path
import subprocess
import time
from append_history_r2 import BATCHES,sha,prefix_matches,write_json
from append_large_dev import rows,remove_owned,disk_bytes
from bounded_solid_dev import CAPS


def run(tool,data,manifest,reference,work,results,task_root,caps=None,sample_count=12):
    selected=rows(data,manifest)
    caps=list(caps or CAPS)
    if len(caps)<2 or len(set(caps))!=len(caps) or any(not 1<=c<=1024**3 for c in caps):
        raise ValueError('invalid caps')
    if max(r['size'] for r in selected)>min(caps):
        raise ValueError('file exceeds requested cap')
    prior={a['label']:a for a in json.loads(reference.read_text(encoding='utf-8'))}
    work.mkdir()
    results.mkdir()
    logs=results/'commands';logs.mkdir()
    affinity={min(os.sched_getaffinity(0))} if hasattr(os,'sched_getaffinity') else None
    total=sum(r['size'] for r in selected)
    orders=[caps[r:]+caps[:r] for r in range(len(caps))]
    write_json(results/'config.json',dict(manifest_sha256=sha(manifest),reference_sha256=sha(reference),
        tool_sha256=sha(tool),runner_sha256=sha(Path(__file__)),bytes=total,files=len(selected),
        frame_limits=caps,orders=orders,rounds=len(caps),sample_count=sample_count,
        operation_order='fresh then append on even rounds; reverse on odd rounds',
        affinity=sorted(affinity) if affinity else None,mode='seed, concrete single group Zstd9',
        cache='prewarm source tree; verify before access; no global cache dropping',
        scope='same dev, warm-cache stability only; no holdout/statistical significance',
        reused_evidence='DEV-021 same C++ / same archive explicit-plan and payload-fault checks'))
    records,artifacts,parents=[],[],{}

    def call(args,label,tree,meta):
        for p in sorted(tree.rglob('*')):
            if p.is_file():
                with p.open('rb') as stream:
                    while stream.read(1024**2):pass
        used=disk_bytes(task_root)
        if used+total+len(selected)*1024+16*1024**2>=1900*1024**2:
            raise RuntimeError('retain output headroom')
        def fixed_cpu():os.sched_setaffinity(0,affinity)
        start=time.perf_counter()
        proc=subprocess.run([str(tool),*map(str,args)],capture_output=True,text=True,encoding='utf-8',
            timeout=1800,preexec_fn=fixed_cpu if affinity else None)
        stem=f'{len(records):03d}-{label}'
        (logs/(stem+'.stdout')).write_text(proc.stdout,encoding='utf-8')
        (logs/(stem+'.stderr')).write_text(proc.stderr,encoding='utf-8')
        item=dict(index=len(records),label=label,argv=list(map(str,args)),meta=meta,
            returncode=proc.returncode,process_wall_ms=(time.perf_counter()-start)*1000,
            disk_before=used,disk_after=disk_bytes(task_root))
        if proc.returncode==0:item['result']=json.loads(proc.stdout)
        records.append(item)
        with (results/'raw.jsonl').open('a',encoding='utf-8') as stream:stream.write(json.dumps(item)+'\n')
        print(json.dumps({k:item[k] for k in ['index','label','returncode','process_wall_ms']}),flush=True)
        if proc.returncode or item['disk_after']>=1900*1024**2:
            raise RuntimeError('command/disk gate failed; preserve evidence')
        return item['result']

    def checked(path,result,label,tree,cap,stage,ref_key,meta,parent=None):
        check=call(['verify',path,tree],label+'-verify',tree,dict(meta,kind='verify'))
        if not check['byte_exact'] or check['generations']!=stage+1:
            raise AssertionError('independent verification failed')
        digest=sha(path)
        ref=prior[ref_key]
        if digest!=ref['sha256'] or path.stat().st_size!=ref['bytes']:
            raise AssertionError('archive differs from pre-registered DEV-021 reference')
        if result['max_frame_raw_bytes']>cap or path.stat().st_size!=result['container_bytes']:
            raise AssertionError('hard cap/physical size failed')
        if parent and (not result['old_is_prefix'] or sha(parent['path'])!=parent['sha256']
            or not prefix_matches(parent['path'],path) or result['old_plaintext_bytes_recompressed']!=0):
            raise AssertionError('fixed parent changed or old plaintext recompressed')
        obj=dict(label=label,meta=meta,cap=cap,bytes=path.stat().st_size,sha256=digest,
                 reference=ref_key,result=result)
        artifacts.append(obj);write_json(results/'archives.json',artifacts)
        return dict(path=path,sha256=digest,bytes=obj['bytes'],result=result)

    # Parent preparation is evidence, kept outside the timed repeat summary.
    for cap in caps:
        cumulative=work/'parent-input';cumulative.mkdir()
        parent=None
        for stage,batch in enumerate(BATCHES[:3]):
            added=work/'prepare-added';added.mkdir()
            for row in selected:
                if row['batch']==batch:
                    for target in [added/row['path'],cumulative/row['path']]:
                        target.parent.mkdir(parents=True,exist_ok=True);os.link(data/row['path'],target)
            path=work/f'parent-{cap}-{stage}.sadx'
            label=f'prepare-{cap}-{stage}'
            meta=dict(phase='prepare',kind='encode',cap=cap,stage=stage)
            args=['bounded-pack','seed',cap,added,path] if parent is None else [
                'bounded-append',cap,parent['path'],added,path]
            result=call(args,label,added,meta)
            next_parent=checked(path,result,label,cumulative,cap,stage,f'{cap}-history-{stage}-seed',meta,parent)
            if parent:parent['path'].unlink()
            parent=next_parent;remove_owned(added,work)
        parents[cap]=parent
        remove_owned(cumulative,work)
    write_json(results/'parents.json',{str(c):{k:v for k,v in p.items() if k!='path'} for c,p in parents.items()})
    added=work/'last-added';added.mkdir()
    for row in selected:
        if row['batch']==BATCHES[3]:
            target=added/row['path'];target.parent.mkdir(parents=True,exist_ok=True);os.link(data/row['path'],target)
    for round_index,order in enumerate(orders):
        for position,cap in enumerate(order):
            operations=['fresh','append'] if round_index%2==0 else ['append','fresh']
            for case_position,operation in enumerate(operations):
                label=f'round-{round_index}-{cap}-{operation}'
                meta=dict(phase='repeat',kind='encode',round=round_index,position=position,
                          operation_position=case_position,operation=operation,cap=cap)
                path=work/'output.sadx'
                args=['bounded-pack','seed',cap,data,path] if operation=='fresh' else [
                    'bounded-append',cap,parents[cap]['path'],added,path]
                result=call(args,label,data if operation=='fresh' else added,meta)
                checked(path,result,label,data,cap,0 if operation=='fresh' else 3,
                    f'{cap}-fresh-seed' if operation=='fresh' else f'{cap}-history-3-seed',meta,
                    parents[cap] if operation=='append' else None)
                access=call(['access-profile',path,data,sample_count],label+'-access',data,dict(meta,kind='access'))
                if not access['byte_exact'] or any(s['decoded_bytes']>cap for s in access['samples']):
                    raise AssertionError('access bound/verification failed')
                path.unlink()
    for parent in parents.values():
        if sha(parent['path'])!=parent['sha256']:raise AssertionError('parent mutated')
        parent['path'].unlink()
    remove_owned(added,work)
    n=len(caps)
    if len(records)!=6*n*(n+1) or len(artifacts)!=3*n+2*n*n:
        raise AssertionError('incomplete balanced matrix')
    write_json(results/'complete.json',dict(commands=len(records),archives_verified=len(artifacts),
        repeat_encodes=2*n*n,access_profiles=2*n*n,rounds=n,all_byte_exact=True,
        all_reference_hashes_matched=True,all_parents_immutable=True))
