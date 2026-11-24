# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit bounded-frame lifecycle measurements and expose each measured tradeoff."""
import json
from pathlib import Path
import sys
from append_history_r2 import BATCHES,sha


def analyze(root,manifest):
    cfg=json.loads((root/'config.json').read_text())
    raw=[json.loads(s) for s in (root/'raw.jsonl').read_text().splitlines()]
    artifacts=json.loads((root/'archives.json').read_text())
    done=json.loads((root/'complete.json').read_text())
    source=[line.split('\t') for line in manifest.read_text().splitlines()]
    caps=cfg['frame_limits']
    n=len(caps)
    if cfg['manifest_sha256']!=sha(manifest) or len(raw)!=32*n or len(artifacts)!=10*n:
        raise ValueError('input or matrix coverage mismatch')
    if [r['index'] for r in raw]!=list(range(len(raw))) or any(r['returncode'] for r in raw):
        raise ValueError('failed/missing commands')
    expected=dict(commands=32*n,archives_verified=10*n,payload_compares=5*n,
                  access_profiles=5*n,fault_profiles=2*n,all_byte_exact=True)
    if done!=expected:
        raise ValueError('completion claims differ')
    if cfg['bytes']!=sum(int(r[1]) for r in source) or cfg['files']!=len(source):
        raise ValueError('source count/bytes differ')
    commands={r['label']:r for r in raw}
    archives={a['label']:a for a in artifacts}
    summary=[]
    for cap in caps:
        history=[]
        previous={}
        for stage in range(5):
            fresh=stage==4
            prefix=f'{cap}-fresh' if fresh else f'{cap}-history-{stage}'
            selected=[r for r in source if fresh or BATCHES.index(r[4])<=stage]
            total=sum(int(r[1]) for r in selected)
            pair=[archives[prefix+'-'+m] for m in ['seed','explicit']]
            s,e=pair
            if s['bytes']!=e['bytes'] or s['result']['parts']!=e['result']['parts']:
                raise ValueError('representation changed size or parts')
            for mode,a in zip(['seed','explicit'],pair):
                result=a['result']
                check=commands[prefix+'-'+mode+'-verify']['result']
                if (not check['byte_exact'] or check['files']!=len(selected)
                    or check['generations']!=(1 if fresh else stage+1)):
                    raise ValueError('independent verification coverage differs')
                if (a['bytes']!=result['container_bytes'] or result['container_revision']!=5
                    or result['strategy_revision']!=2 or result['frame_limit_bytes']!=cap
                    or result['frame_count']!=len(result['frame_raw_bytes'])
                    or sum(result['frame_raw_bytes'])!=total
                    or max(result['frame_raw_bytes'])!=result['max_frame_raw_bytes']
                    or result['max_frame_raw_bytes']>cap
                    or sum(v for k,v in result['parts'].items() if k!='group_metadata')!=a['bytes']):
                    raise ValueError('frame / physical byte accounting failed')
                if stage and not fresh:
                    p=previous[mode]
                    if (not result['old_is_prefix'] or result['old_archive_bytes_read']!=p['bytes']
                        or result['old_archive_bytes_copied']!=p['bytes']
                        or result['old_container_bytes']!=p['bytes']
                        or result['old_plaintext_bytes_recompressed']!=0
                        or result['archive_bytes_written']!=a['bytes']
                        or result['frame_raw_bytes'][:p['result']['frame_count']]!=p['result']['frame_raw_bytes']):
                        raise ValueError('old archive prefix/work accounting failed')
                    added=sum(int(r[1]) for r in source if r[4]==BATCHES[stage])
                    if result['appended_bytes']!=added:
                        raise ValueError('new input size changed')
            if not commands[prefix+'-payload']['result']['equal']:
                raise ValueError('compressed payloads differ')
            access=commands[prefix+'-access']['result']
            samples=access['samples']
            ordered=sorted(selected,key=lambda r:r[0])
            count=min(cfg['sample_count'],len(ordered))
            indices={0 if count==1 else i*(len(ordered)-1)//(count-1) for i in range(count)}
            indices.add(max(range(len(ordered)),key=lambda i:int(ordered[i][1])))
            if [s['path'] for s in samples]!=[ordered[i][0] for i in sorted(indices)] or not access['byte_exact']:
                raise ValueError('access sample selection changed')
            sizes={r[0]:int(r[1]) for r in selected}
            for sample in samples:
                if (sample['requested_bytes']!=sizes[sample['path']]
                    or sample['decoded_bytes']!=s['result']['frame_raw_bytes'][sample['frame']]
                    or not sample['requested_bytes']<=sample['decoded_bytes']<=cap
                    or sample['archive_bytes_read']!=s['bytes']):
                    raise ValueError('access I/O/decode accounting differs')
            for output,key in [('requested_bytes_total','requested_bytes'),('decoded_bytes_total','decoded_bytes'),
                               ('archive_bytes_read_total','archive_bytes_read')]:
                if access[output]!=sum(t[key] for t in samples):
                    raise ValueError('access total differs')
            times=sorted(t['wall_ms'] for t in samples)
            for key,p in [('median_ms',50),('p95_ms',95)]:
                value=times[(p*(len(times)-1)+50)//100]
                if abs(value-access[key])>max(.01,abs(value)*1e-5):
                    raise ValueError('access quantile differs')
            fault=None
            if stage in [3,4]:
                fault=commands[prefix+'-fault']['result']
                if (fault['wrong_bytes_accepted']!=0 or fault['outputs_written']!=0
                    or not fault['verified_source'] or fault['input_bytes']!=total
                    or [c['payload_percent'] for c in fault['cases']]!=[10,50,90]):
                    raise ValueError('fault assumptions/coverage differ')
                for case in fault['cases']:
                    if (case['verified_bytes']+case['unavailable_bytes']!=total
                        or case['verified_files']+case['rejected_files']!=len(selected)
                        or not 0<=case['damage_offset']<s['bytes']
                        or case['frame_file_bytes_at_risk']!=s['result']['frame_raw_bytes'][case['frame']]
                        or (case['index_usable'] and case['unavailable_bytes']>case['frame_file_bytes_at_risk'])):
                        raise ValueError('fault isolation/byte accounting failed')
            history.append(dict(kind='fresh' if fresh else 'history',stage=stage if not fresh else None,
                input_bytes=total,bytes=s['bytes'],parts=s['result']['parts'],frames=s['result']['frame_count'],
                max_frame_bytes=s['result']['max_frame_raw_bytes'],
                seed_call_seconds=s['result']['call_wall_ms']/1000,
                explicit_call_seconds=e['result']['call_wall_ms']/1000,
                seed_verify_seconds=commands[prefix+'-seed-verify']['process_wall_ms']/1000,
                access_median_ms=access['median_ms'],access_p95_ms=access['p95_ms'],
                access_decoded_bytes_total=access['decoded_bytes_total'],
                access_archive_bytes_read_total=access['archive_bytes_read_total'],
                old_archive_bytes_read=s['result'].get('old_archive_bytes_read',0),
                old_archive_bytes_copied=s['result'].get('old_archive_bytes_copied',0),
                fault_cases=fault['cases'] if fault else None,seed_sha256=s['sha256'],explicit_sha256=e['sha256']))
            if not fresh:
                previous=dict(zip(['seed','explicit'],pair))
        summary.append(dict(frame_limit=cap,stages=history,
            lifecycle_seed_encode_seconds=sum(s['seed_call_seconds'] for s in history[:4]),
            lifecycle_explicit_encode_seconds=sum(s['explicit_call_seconds'] for s in history[:4])))
    return dict(status='independently audited from raw commands, manifest and archive ledger',
        **expected,configs=summary,peak_memory_bytes=max(r['result'].get('peak_memory_bytes',0) for r in raw),
        observed_disk_peak_bytes=max(max(r['disk_before'],r['disk_after']) for r in raw),
        limitations='one dev observation; Zstd9 only; full archive I/O; intact-index payload faults only')


if __name__=='__main__':
    result=analyze(Path(sys.argv[1]),Path(sys.argv[2]))
    Path(sys.argv[3]).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(status=result['status'],commands=result['commands'],archives=result['archives_verified'])))
