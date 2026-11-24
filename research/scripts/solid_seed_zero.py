# SPDX-License-Identifier: GPL-3.0-or-later
"""512 MiB dev zero-search payload equivalence, not a compression gain trial."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from append_history_r2 import BATCHES, sha, prefix_matches, write_json
from append_large_dev import rows, remove_owned, disk_bytes


def run(tool, data, manifest, work, results, task_root):
    selected=rows(data,manifest)
    work.mkdir(exist_ok=False)
    results.mkdir(exist_ok=False)
    commands=results/'commands'
    commands.mkdir()
    modes=['solid','seed-solid','explicit-solid']
    parents={}
    history=[]
    records=[]
    affinity={min(os.sched_getaffinity(0))}
    write_json(results/'config.json',dict(scope='zero-search equivalence only',
        tool_sha256=sha(tool),manifest_sha256=sha(manifest),plan='0x0',
        strategy='solid-plan-r1 / revision2, one group, Zstd9',modes=modes,
        bytes=sum(r['size'] for r in selected),files=len(selected),affinity=sorted(affinity),
        search_commands=0,input='same Linux 6.12 dev contents and paths as prior large run'))
    def call(args,label):
        before=disk_bytes(task_root)
        if before>1800*1024**2:
            raise RuntimeError('retain task disk headroom')
        def fixed_cpu():
            os.sched_setaffinity(0,affinity)
        start=time.perf_counter()
        proc=subprocess.run([str(tool),*map(str,args)],capture_output=True,text=True,
                            timeout=1200,preexec_fn=fixed_cpu)
        stem=f'{len(records):03d}-{label}'
        (commands/(stem+'.stdout')).write_text(proc.stdout)
        (commands/(stem+'.stderr')).write_text(proc.stderr)
        item=dict(index=len(records),label=label,argv=list(map(str,args)),
                  returncode=proc.returncode,process_wall_ms=(time.perf_counter()-start)*1000,
                  disk_before=before,disk_after=disk_bytes(task_root))
        if proc.returncode==0:
            item['result']=json.loads(proc.stdout)
        records.append(item)
        with (results/'raw.jsonl').open('a') as stream:
            stream.write(json.dumps(item)+'\n')
        print(json.dumps({k:item[k] for k in ['index','label','returncode','process_wall_ms']}),flush=True)
        if proc.returncode or item['disk_after']>1900*1024**2:
            raise RuntimeError('zero-search command or disk gate failed; preserve artifacts')
        return item['result']
    cumulative=work/'cumulative'
    cumulative.mkdir()
    for stage,batch in enumerate(BATCHES):
        added=work/'added'
        added.mkdir()
        for row in selected:
            if row['batch']==batch:
                dest=added/row['path']
                dest.parent.mkdir(parents=True,exist_ok=True)
                os.link(data/row['path'],dest)
                dest=cumulative/row['path']
                dest.parent.mkdir(parents=True,exist_ok=True)
                os.link(data/row['path'],dest)
        current={}
        for mode in modes:
            archive=work/f'{stage}-{mode}.sadx'
            if stage:
                item=call(['append',parents[mode]['path'],added,archive,'segment'],f'{stage}-{mode}-append')
                if not item.get('old_is_prefix') or not prefix_matches(parents[mode]['path'],archive):
                    raise AssertionError('lost exact old prefix')
                if sha(parents[mode]['path'])!=parents[mode]['sha256']:
                    raise AssertionError('old archive changed')
            else:
                item=call(['pack',mode,'0x0',added,archive,'off'],f'{stage}-{mode}-pack')
            checked=call(['verify',archive,cumulative],f'{stage}-{mode}-verify')
            if not checked.get('byte_exact') or checked['generations']!=stage+1:
                raise AssertionError('roundtrip or generation mismatch')
            current[mode]=dict(path=archive,sha256=sha(archive),bytes=archive.stat().st_size,parts=item['parts'])
        for mode in ['solid','explicit-solid']:
            comparison=call(['payload-compare',current['seed-solid']['path'],current[mode]['path']],f'{stage}-compare-{mode}')
            if not comparison['equal']:
                raise AssertionError('different compression payload')
        snapshot=dict(stage=stage,archives={mode:{k:v for k,v in obj.items() if k!='path'} for mode,obj in current.items()},
                      all_payloads_equal=True,search_commands=0)
        history.append(snapshot)
        with (results/'histories.jsonl').open('a') as stream:
            stream.write(json.dumps(snapshot)+'\n')
        for obj in parents.values():
            obj['path'].unlink()
        parents=current
        remove_owned(added,work)
    # Regroup shares the same plan in all generations; compare it separately.
    third=work/'third'
    third.mkdir()
    extra=third/'新增零搜索.txt'
    extra.write_text('one last append for regroup equivalence\n')
    shutil.copyfile(extra,cumulative/extra.name)
    regroup={}
    for mode in modes:
        out=work/f'{mode}-regroup.sadx'
        call(['append',parents[mode]['path'],third,out,'regroup'],mode+'-regroup')
        checked=call(['verify',out,cumulative],mode+'-regroup-verify')
        if not checked.get('byte_exact') or checked['generations']!=1:
            raise AssertionError('regroup verification failed')
        regroup[mode]=out
        parents[mode]['path'].unlink()
    for mode in ['solid','explicit-solid']:
        call(['payload-compare',regroup['seed-solid'],regroup[mode]],'regroup-compare-'+mode)
    result=dict(scope='zero-search dev payload equivalence, no search benefit conclusion',
                stages=4,commands=len(records),archives_verified=15,payload_comparisons=10,
                search_commands=0,all_payloads_equal=True,byte_exact=True,
                final_segment_bytes={m:history[-1]['archives'][m]['bytes'] for m in modes})
    write_json(results/'complete.json',result)
    for path in regroup.values():
        path.unlink()
    remove_owned(third,work)
    remove_owned(cumulative,work)
    return result
