# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit round balance, exact reference outputs and paired descriptive statistics."""
import json
from pathlib import Path
import statistics
import sys
from append_history_r2 import BATCHES,sha


def stats(values):
    return dict(n=len(values),min=min(values),median=statistics.median(values),max=max(values))


def analyze(root,manifest,reference):
    cfg=json.loads((root/'config.json').read_text())
    raw=[json.loads(s) for s in (root/'raw.jsonl').read_text().splitlines()]
    archives=json.loads((root/'archives.json').read_text())
    refs={a['label']:a for a in json.loads(reference.read_text())}
    complete=json.loads((root/'complete.json').read_text())
    caps=cfg['frame_limits'];n=len(caps)
    source=[s.split('\t') for s in manifest.read_text().splitlines()]
    total=sum(int(r[1]) for r in source)
    if (cfg['manifest_sha256']!=sha(manifest) or cfg['reference_sha256']!=sha(reference)
        or cfg['bytes']!=total or cfg['files']!=len(source) or cfg['rounds']!=n
        or cfg['orders']!=[caps[r:]+caps[:r] for r in range(n)]):
        raise ValueError('input or balanced schedule changed')
    expected_meta=[]
    for cap in caps:
        for stage in range(3):
            for kind in ['encode','verify']:
                expected_meta.append(dict(phase='prepare',kind=kind,cap=cap,stage=stage))
    for r,order in enumerate(cfg['orders']):
        for position,cap in enumerate(order):
            operations=['fresh','append'] if r%2==0 else ['append','fresh']
            for op_pos,operation in enumerate(operations):
                for kind in ['encode','verify','access']:
                    expected_meta.append(dict(phase='repeat',kind=kind,round=r,position=position,
                        operation_position=op_pos,operation=operation,cap=cap))
    if (len(raw)!=len(expected_meta) or [r['index'] for r in raw]!=list(range(len(raw)))
        or [r['meta'] for r in raw]!=expected_meta or any(r['returncode'] for r in raw)):
        raise ValueError('failed/missing/reordered commands')
    expected_done=dict(commands=6*n*(n+1),archives_verified=3*n+2*n*n,repeat_encodes=2*n*n,
        access_profiles=2*n*n,rounds=n,all_byte_exact=True,all_reference_hashes_matched=True,all_parents_immutable=True)
    if complete!=expected_done or len(archives)!=expected_done['archives_verified']:
        raise ValueError('completion claims differ')
    commands={r['label']:r for r in raw}
    parents=json.loads((root/'parents.json').read_text())
    for cap in caps:
        p=parents[str(cap)];ref=refs[f'{cap}-history-2-seed']
        if p['sha256']!=ref['sha256'] or p['bytes']!=ref['bytes']:
            raise ValueError('parent did not match generation3 reference')
    observations=[]
    preparation=[]
    ordered=sorted(source,key=lambda r:r[0]);count=min(cfg['sample_count'],len(ordered))
    indices={0 if count==1 else i*(len(ordered)-1)//(count-1) for i in range(count)}
    indices.add(max(range(len(ordered)),key=lambda i:int(ordered[i][1])))
    sample_paths=[ordered[i][0] for i in sorted(indices)]
    sizes={r[0]:int(r[1]) for r in source}
    for a in archives:
        item=commands[a['label']];meta=a['meta'];result=a['result'];cap=a['cap']
        if item['meta']!=meta or item['result']!=result:
            raise ValueError('artifact does not match command record')
        ref=refs[a['reference']]
        if (a['sha256']!=ref['sha256'] or a['bytes']!=ref['bytes'] or result['parts']!=ref['result']['parts']
            or result['frame_raw_bytes']!=ref['result']['frame_raw_bytes']):
            raise ValueError('reference bytes or parts changed')
        selected=[r for r in source if BATCHES.index(r[4])<=meta['stage']] if meta['phase']=='prepare' else source
        verify=commands[a['label']+'-verify']['result']
        generations=meta['stage']+1 if meta['phase']=='prepare' else 1 if meta['operation']=='fresh' else 4
        if (not verify['byte_exact'] or verify['files']!=len(selected) or verify['generations']!=generations
            or result['container_bytes']!=a['bytes'] or result['container_revision']!=5
            or result['strategy_revision']!=2 or result['frame_limit_bytes']!=cap
            or result['frame_count']!=len(result['frame_raw_bytes'])
            or sum(result['frame_raw_bytes'])!=sum(int(r[1]) for r in selected)
            or max(result['frame_raw_bytes'])!=result['max_frame_raw_bytes'] or result['max_frame_raw_bytes']>cap
            or sum(v for k,v in result['parts'].items() if k!='group_metadata')!=a['bytes']):
            raise ValueError('archive/cap/verification accounting failed')
        if meta['phase']=='prepare':
            preparation.append(dict(cap=cap,stage=meta['stage'],call_seconds=result['call_wall_ms']/1000,
                                    bytes=a['bytes'],sha256=a['sha256']))
            continue
        if meta['operation']=='append':
            p=parents[str(cap)]
            if (not result['old_is_prefix'] or result['old_archive_bytes_read']!=p['bytes']
                or result['old_archive_bytes_copied']!=p['bytes'] or result['old_container_bytes']!=p['bytes']
                or result['old_plaintext_bytes_recompressed']!=0 or result['archive_bytes_written']!=a['bytes']
                or result['appended_bytes']!=sum(int(r[1]) for r in source if r[4]=='append3')
                or result['frame_raw_bytes'][:p['result']['frame_count']]!=p['result']['frame_raw_bytes']):
                raise ValueError('append old work/new input differs')
        elif result['input_bytes']!=total:
            raise ValueError('fresh input differs')
        access=commands[a['label']+'-access']['result']
        samples=access['samples']
        if not access['byte_exact'] or [s['path'] for s in samples]!=sample_paths or access['sample_count']!=len(samples):
            raise ValueError('access sample set changed')
        for sample in samples:
            if (sample['requested_bytes']!=sizes[sample['path']]
                or sample['decoded_bytes']!=result['frame_raw_bytes'][sample['frame']]
                or sample['archive_bytes_read']!=a['bytes'] or sample['decoded_bytes']>cap):
                raise ValueError('access byte accounting differs')
        for output,key in [('requested_bytes_total','requested_bytes'),('decoded_bytes_total','decoded_bytes'),
                           ('archive_bytes_read_total','archive_bytes_read')]:
            if access[output]!=sum(s[key] for s in samples):raise ValueError('access total differs')
        times=sorted(s['wall_ms'] for s in samples)
        for key,p in [('median_ms',50),('p95_ms',95)]:
            value=times[(p*(len(times)-1)+50)//100]
            if abs(value-access[key])>max(.01,abs(value)*1e-5):raise ValueError('access quantile differs')
        observations.append(dict(round=meta['round'],position=meta['position'],operation_position=meta['operation_position'],
            cap=cap,operation=meta['operation'],bytes=a['bytes'],sha256=a['sha256'],
            call_seconds=result['call_wall_ms']/1000,cpu_seconds=result['call_cpu_ms']/1000,
            process_seconds=item['process_wall_ms']/1000,verify_seconds=commands[a['label']+'-verify']['process_wall_ms']/1000,
            access_median_ms=access['median_ms'],access_p95_ms=access['p95_ms'],
            access_decoded_bytes_total=access['decoded_bytes_total'],frames=result['frame_count']))
    baseline=1024**3
    summaries=[]
    for cap in caps:
        for operation in ['fresh','append']:
            group=sorted([r for r in observations if r['cap']==cap and r['operation']==operation],key=lambda r:r['round'])
            if len(group)!=n or len({r['sha256'] for r in group})!=1:raise ValueError('unstable output or missing round')
            ratios=[]
            for row in group:
                base=next(r for r in observations if r['round']==row['round'] and r['cap']==baseline and r['operation']==operation)
                ratios.append(dict(round=row['round'],access_speedup=base['access_median_ms']/row['access_median_ms'],
                                   encode_speedup=base['call_seconds']/row['call_seconds']))
            base=next(r for r in observations if r['cap']==baseline and r['operation']==operation)
            summaries.append(dict(cap=cap,operation=operation,bytes=group[0]['bytes'],sha256=group[0]['sha256'],
                size_overhead_percent=(group[0]['bytes']/base['bytes']-1)*100,
                call_seconds=stats([r['call_seconds'] for r in group]),
                access_median_ms=stats([r['access_median_ms'] for r in group]),
                access_p95_ms=stats([r['access_p95_ms'] for r in group]),
                paired_ratios=ratios,paired_access_speedup=stats([r['access_speedup'] for r in ratios]),
                paired_encode_speedup=stats([r['encode_speedup'] for r in ratios]),
                access_decoded_bytes_total=group[0]['access_decoded_bytes_total']))
    return dict(status='independently audited balanced repeats',**expected_done,preparation=preparation,
        observations=observations,summaries=summaries,
        peak_memory_bytes=max(r['result'].get('peak_memory_bytes',0) for r in raw),
        observed_disk_peak_bytes=max(max(r['disk_before'],r['disk_after']) for r in raw),
        limitations=f'{n} same-dev warm-cache observations per case; no cold disk or statistical significance / cross-corpus guarantee.')


if __name__=='__main__':
    a=analyze(Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]))
    Path(sys.argv[4]).write_text(json.dumps(a,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(status=a['status'],commands=a['commands'],archives=a['archives_verified'])))
