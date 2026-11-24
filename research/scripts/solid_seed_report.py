# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the zero-search run and attribute metadata separately from payload."""
import argparse
import json
from pathlib import Path

from append_history_r2 import sha


def analyze(root, manifest, regroup_hashes=None):
    config=json.loads((root/'config.json').read_text())
    done=json.loads((root/'complete.json').read_text())
    raw=list(map(json.loads,(root/'raw.jsonl').read_text().splitlines()))
    histories=list(map(json.loads,(root/'histories.jsonl').read_text().splitlines()))
    if sha(manifest)!=config['manifest_sha256'] or len(raw)!=40 or done['commands']!=40:
        raise ValueError('input or command count mismatch')
    if [r['index'] for r in raw]!=list(range(40)) or any(r['returncode']!=0 for r in raw):
        raise ValueError('failed or missing command')
    if any(r['argv'][0]=='search' for r in raw):
        raise ValueError('unexpected strategy search')
    labels={r['label']:r for r in raw}
    physical=['header','generation_records','dictionaries','data','redundancy_records',
              'stale_trailers','trailer','footer']
    table=[]
    if [r['stage'] for r in histories]!=[0,1,2,3]:
        raise ValueError('missing stage')
    for snapshot in histories:
        stage=snapshot['stage']
        archives=snapshot['archives']
        for mode,entry in archives.items():
            result=labels[f'{stage}-{mode}-'+('pack' if stage==0 else 'append')]['result']
            verified=labels[f'{stage}-{mode}-verify']['result']
            if (not verified.get('byte_exact') or verified['generations']!=stage+1 or
                    result['container_bytes']!=entry['bytes'] or
                    sum(entry['parts'][k] for k in physical)!=entry['bytes'] or
                    len(entry['sha256'])!=64):
                raise ValueError('archive verification or physical accounting failed')
            if result['container_revision']!=(4 if mode=='solid' else 5):
                raise ValueError('wrong container revision')
            if mode!='solid' and result['strategy_revision']!=2:
                raise ValueError('wrong strategy revision')
        for mode in ['solid','explicit-solid']:
            compared=labels[f'{stage}-compare-{mode}']['result']
            if (not compared['equal'] or compared['left_container_bytes']!=archives['seed-solid']['bytes']
                    or compared['right_container_bytes']!=archives[mode]['bytes']):
                raise ValueError('payload comparison failed')
        old,new,explicit=[archives[m] for m in ['solid','seed-solid','explicit-solid']]
        n=stage+1
        expected_extra=4*(n+n*(n+1)//2)
        if new['bytes']!=explicit['bytes'] or new['bytes']-old['bytes']!=expected_extra:
            raise ValueError('unexpected representation overhead')
        if not old['parts']['data']==new['parts']['data']==explicit['parts']['data']:
            raise ValueError('different payload totals')
        table.append(dict(stage=stage,legacy_bytes=old['bytes'],seed_bytes=new['bytes'],
                          explicit_bytes=explicit['bytes'],payload_bytes=new['parts']['data'],
                          extra_metadata_bytes=expected_extra))
    regroup={}
    for mode in ['solid','seed-solid','explicit-solid']:
        result=labels[mode+'-regroup']['result']
        verified=labels[mode+'-regroup-verify']['result']
        if not verified.get('byte_exact') or verified['generations']!=1:
            raise ValueError('regroup roundtrip failed')
        if sum(result['parts'][k] for k in physical)!=result['container_bytes']:
            raise ValueError('regroup physical accounting failed')
        regroup[mode]=result['container_bytes']
    for mode in ['solid','explicit-solid']:
        if not labels['regroup-compare-'+mode]['result']['equal']:
            raise ValueError('regroup payload differs')
    if regroup['seed-solid']!=regroup['explicit-solid'] or regroup['seed-solid']-regroup['solid']!=8:
        raise ValueError('regroup metadata mismatch')
    hash_count=0
    if regroup_hashes:
        observed=json.loads(regroup_hashes.read_text())
        if not observed['complete'] or set(observed['archives'])!=set(regroup):
            raise ValueError('incomplete independent regroup hashes')
        for mode,entry in observed['archives'].items():
            if entry['bytes']!=regroup[mode] or len(entry['sha256'])!=64 or entry['verified_label']!=mode+'-regroup-verify':
                raise ValueError('independent regroup hash ledger mismatch')
        hash_count=3
    return dict(scope='zero-search functional equivalence, not search/ratio improvement',
        manifest_sha256=config['manifest_sha256'],tool_sha256=config['tool_sha256'],input_bytes=config['bytes'],files=config['files'],
        validated=dict(commands=40,archives_verified=15,payload_comparisons=10,searches=0),
        stages=table,regroup_bytes=regroup,
        full_archive_sha256_coverage=dict(segment_outputs=12,regroup_outputs=hash_count,
            note='Independent read-only observer hashes after verify when provided; otherwise regroup whole-file hashes were not retained.'),
        peak_tool_rss_bytes=max(r['result'].get('peak_memory_bytes',0) for r in raw),
        peak_observed_task_disk=max(max(r['disk_before'],r['disk_after']) for r in raw),
        limitations=['Default one-group plan only at large scale; four grouping plans checked in small acceptance.',
                     'One dev run and fixed call order; no timing superiority or statistical conclusion.',
                     'No solid strategy search, B-mix, recovery or volumes in the new profile.',
                     'Input/archive/frame remain memory-resident; no >1 GiB capability claimed.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results',type=Path)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--regroup-hashes',type=Path)
    args=parser.parse_args()
    if args.output.exists():
        raise FileExistsError('do not overwrite analysis')
    result=analyze(args.results,args.manifest,args.regroup_hashes)
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
