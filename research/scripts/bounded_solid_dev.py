# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent bounded-frame histories, with complete operation and access accounting."""
import json
import os
from pathlib import Path
import subprocess
import time
from append_history_r2 import BATCHES, sha, prefix_matches, write_json
from append_large_dev import rows, remove_owned, disk_bytes

CAPS=[1024**3,32*1024**2,64*1024**2,128*1024**2]


def run(tool,data,manifest,work,results,task_root,caps=None,sample_count=12):
    selected=rows(data,manifest)
    caps=list(caps or CAPS)
    if not caps or len(set(caps))!=len(caps) or any(not 1<=c<=1024**3 for c in caps):
        raise ValueError('invalid frame limits')
    if max(r['size'] for r in selected)>min(caps):
        raise ValueError('one input exceeds a pre-registered hard frame limit')
    work.mkdir()
    results.mkdir()
    commands=results/'commands'
    commands.mkdir()
    affinity={min(os.sched_getaffinity(0))} if hasattr(os,'sched_getaffinity') else None
    total=sum(r['size'] for r in selected)
    write_json(results/'config.json',dict(tool_sha256=sha(tool),runner_sha256=sha(Path(__file__)),
        manifest_sha256=sha(manifest),bytes=total,files=len(selected),frame_limits=caps,
        modes=['seed','explicit'],plan='single group / revision2, Zstd9',repetitions=1,
        order='one full history per cap, seed then explicit; not randomized',
        sample_count=sample_count,selection='uniform sorted paths plus largest file, deduplicated',
        affinity=sorted(affinity) if affinity else None,cache='prewarm each command input; no global cache drop',
        scope='dev one observation; no holdout or statistical superiority',
        io='input and old archive fully loaded; all old archive bytes copied for append',
        fault='one-bit flips at10/50/90 percent of compressed payload; intact metadata only'))
    records,artifacts=[],[]

    def call(args,label,tree):
        for p in sorted(tree.rglob('*')):
            if p.is_file():
                with p.open('rb') as stream:
                    while stream.read(1024**2):
                        pass
        used=disk_bytes(task_root)
        if used+total+len(selected)*1024+16*1024**2>=1900*1024**2:
            raise RuntimeError('retain conservative output headroom')
        def fixed_cpu():
            os.sched_setaffinity(0,affinity)
        start=time.perf_counter()
        proc=subprocess.run([str(tool),*map(str,args)],capture_output=True,text=True,encoding='utf-8',
            timeout=1800,preexec_fn=fixed_cpu if affinity else None)
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
            raise RuntimeError('command or disk gate failed; preserve artifacts')
        return item['result']

    def check_archive(path,result,label,tree,cap,generations,parent=None):
        verified=call(['verify',path,tree],label+'-verify',tree)
        if not verified['byte_exact'] or verified['generations']!=generations:
            raise AssertionError('independent full verification failed')
        if (path.stat().st_size!=result['container_bytes'] or result['max_frame_raw_bytes']>cap
            or result['frame_count']!=len(result['frame_raw_bytes'])
            or result['max_frame_raw_bytes']!=max(result['frame_raw_bytes'])):
            raise AssertionError('frame cap/physical output failed')
        if parent and (not result['old_is_prefix'] or not prefix_matches(parent['path'],path)
            or sha(parent['path'])!=parent['sha256'] or result['old_plaintext_bytes_recompressed']!=0
            or result['old_archive_bytes_read']!=parent['bytes']
            or result['old_archive_bytes_copied']!=parent['bytes']):
            raise AssertionError('append history / old work accounting failed')
        obj=dict(label=label,cap=cap,bytes=path.stat().st_size,sha256=sha(path),result=result)
        artifacts.append(obj)
        write_json(results/'archives.json',artifacts)
        return dict(path=path,sha256=obj['sha256'],bytes=obj['bytes'])

    def access(path,tree,cap,label):
        result=call(['access-profile',path,tree,sample_count],label+'-access',tree)
        if (not result['byte_exact'] or any(s['decoded_bytes']>cap or s['decoded_bytes']<s['requested_bytes']
            for s in result['samples'])):
            raise AssertionError('access bound failed')

    for cap in caps:
        branch=work/str(cap)
        branch.mkdir()
        cumulative=branch/'cumulative'
        cumulative.mkdir()
        parents={}
        for stage,batch in enumerate(BATCHES):
            added=branch/'added'
            added.mkdir()
            for row in selected:
                if row['batch']==batch:
                    for target in [added/row['path'],cumulative/row['path']]:
                        target.parent.mkdir(parents=True,exist_ok=True)
                        os.link(data/row['path'],target)
            current={}
            label=f'{cap}-history-{stage}'
            for mode in ['seed','explicit']:
                path=branch/f'{stage}-{mode}.sadx'
                args=['bounded-pack',mode,cap,added,path] if not stage else [
                    'bounded-append',cap,parents[mode]['path'],added,path]
                result=call(args,label+'-'+mode,added)
                current[mode]=check_archive(path,result,label+'-'+mode,cumulative,cap,stage+1,parents.get(mode))
            comparison=call(['payload-compare',current['seed']['path'],current['explicit']['path']],label+'-payload',cumulative)
            if not comparison['equal']:
                raise AssertionError('representation changed compressed frames')
            access(current['seed']['path'],cumulative,cap,label)
            if stage==3:
                call(['payload-fault-profile',current['seed']['path'],cumulative],label+'-fault',cumulative)
            for obj in parents.values():
                obj['path'].unlink()
            parents=current
            remove_owned(added,branch)
        for obj in parents.values():
            obj['path'].unlink()
        label=f'{cap}-fresh'
        fresh={}
        for mode in ['seed','explicit']:
            path=branch/f'fresh-{mode}.sadx'
            result=call(['bounded-pack',mode,cap,cumulative,path],label+'-'+mode,cumulative)
            fresh[mode]=check_archive(path,result,label+'-'+mode,cumulative,cap,1)
        comparison=call(['payload-compare',fresh['seed']['path'],fresh['explicit']['path']],label+'-payload',cumulative)
        if not comparison['equal']:
            raise AssertionError('fresh representation frames differ')
        access(fresh['seed']['path'],cumulative,cap,label)
        call(['payload-fault-profile',fresh['seed']['path'],cumulative],label+'-fault',cumulative)
        for obj in fresh.values():
            obj['path'].unlink()
        remove_owned(cumulative,branch)
        branch.rmdir()
    if len(records)!=32*len(caps) or len(artifacts)!=10*len(caps):
        raise AssertionError('incomplete pre-registered matrix')
    write_json(results/'complete.json',dict(commands=len(records),archives_verified=len(artifacts),
        payload_compares=5*len(caps),access_profiles=5*len(caps),fault_profiles=2*len(caps),
        all_byte_exact=True))
