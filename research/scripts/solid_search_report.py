# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit all search ledger entries and the complete fixed-host dev matrix."""
import json
from pathlib import Path
import sys
from append_history_r2 import sha


def audit_search(r):
    trials = r['trials']
    unit = max(r['input_bytes'],1)
    n = len(trials)
    if not 1 <= n <= 29 or n != r['evaluations'] or r['domain_size'] != 29:
        raise ValueError('invalid candidate coverage')
    if r['budget_spent_bytes'] != n*unit or n*unit > r['budget_bytes']:
        raise ValueError('unaccounted encoding or budget overrun')
    if trials[0]['plan'] != r['start_plan'] or trials[0]['bytes'] != r['start_bytes']:
        raise ValueError('history did not start at declared plan')
    def pkey(p): return p['rule'], p['groups']
    def tkey(t): return t['bytes'], *pkey(t['plan'])
    if len({pkey(t['plan']) for t in trials}) != n:
        raise ValueError('duplicate charged trial')
    best = min(trials,key=tkey)
    if best['bytes'] != r['best_bytes'] or best['plan'] != r['best_plan']:
        raise ValueError('chosen output is not minimum full archive / deterministic tie')
    if r['container_bytes'] != r['best_bytes'] or not r['byte_exact']:
        raise ValueError('published output differs from searched winner')
    fixed = [t for t in trials if pkey(t['plan'])==(0,1)]
    if len(fixed)!=1 or fixed[0]['bytes']!=r['baseline_bytes']:
        raise ValueError('strong default missing')
    expected=[]
    incumbent=None
    for i,t in enumerate(trials,1):
        if t['evaluation'] != i or t['spent_bytes'] != i*unit:
            raise ValueError('trial ledger discontinuity')
        if incumbent is None or tkey(t)<tkey(incumbent):
            incumbent=t
            expected.append(dict(evaluations=i,spent_bytes=i*unit,best_bytes=t['bytes'],plan=t['plan']))
    if expected != r['trajectory']:
        raise ValueError('best trajectory disagrees with independently computed minima')
    observed=sorted([dict(plan=t['plan'],bytes=t['bytes']) for t in trials],key=lambda t:pkey(t['plan']))
    if observed!=r['evaluated'] or r['space_exhausted']!=(n==29):
        raise ValueError('candidate summary differs')
    return dict(plan=r['best_plan'],evaluations=n,budget_spent_bytes=r['budget_spent_bytes'],
                gain_from_default_bytes=r['baseline_bytes']-r['best_bytes'],
                gain_from_start_bytes=r['start_bytes']-r['best_bytes'])


def analyze(root,manifest):
    config=json.loads((root/'config.json').read_text())
    raw=[json.loads(s) for s in (root/'raw.jsonl').read_text().splitlines()]
    archives=json.loads((root/'archives.json').read_text())
    complete=json.loads((root/'complete.json').read_text())
    if config['manifest_sha256']!=sha(manifest) or len(raw)!=25 or len(archives)!=10:
        raise ValueError('wrong input or incomplete matrix')
    if [r['index'] for r in raw]!=list(range(25)) or any(r['returncode'] for r in raw):
        raise ValueError('failed/missing command')
    searches=[r for r in raw if r['argv'][0]=='solid-search']
    verifies=[r for r in raw if r['argv'][0]=='verify']
    compares=[r for r in raw if r['argv'][0]=='payload-compare']
    if len(searches)!=6 or len(verifies)!=10 or len(compares)!=5:
        raise ValueError('missing comparison coverage')
    if any(not r['result']['byte_exact'] for r in verifies) or any(not r['result']['equal'] for r in compares):
        raise ValueError('verification failed')
    audits={r['label']:audit_search(r['result']) for r in searches}
    by_label={r['label']:r for r in raw}
    table=[]
    for variant in ['initial','fixed','reuse','warm','cold']:
        pair=[next(a for a in archives if a['label']==variant+'-'+m) for m in ['seed','explicit']]
        s,e=pair
        if s['bytes']!=e['bytes'] or s['result']['parts']!=e['result']['parts']:
            raise ValueError('different representation sizes/parts')
        for a in pair:
            r=a['result']
            parts=r['parts']
            if (a['bytes']!=r['container_bytes'] or r['container_revision']!=5 or r['strategy_revision']!=2
                or sum(v for k,v in parts.items() if k!='group_metadata')!=a['bytes']):
                raise ValueError('physical archive parts do not reconcile')
        if variant in ['initial','warm','cold']:
            for field in ['start_plan','best_plan','evaluated','budget_spent_bytes']:
                if s['result'][field]!=e['result'][field]:
                    raise ValueError('same-operator representation difference')
        if variant=='warm':
            for m,a in zip(['seed','explicit'],pair):
                prior=next(p for p in archives if p['label']=='initial-'+m)
                if a['result']['start_plan']!=prior['result']['best_plan']:
                    raise ValueError('warm history was not inherited')
        table.append(dict(variant=variant,bytes=s['bytes'],payload_bytes=s['result']['parts']['data'],
            metadata_bytes=s['bytes']-s['result']['parts']['data'],
            seed_tool_seconds=by_label[s['label']]['result']['call_wall_ms']/1000,
            explicit_tool_seconds=by_label[e['label']]['result']['call_wall_ms']/1000,
            seed_external_verify_seconds=by_label[s['label']+'-verify']['process_wall_ms']/1000,
            explicit_external_verify_seconds=by_label[e['label']+'-verify']['process_wall_ms']/1000,
            plan=s['result'].get('best_plan'),seed_sha256=s['sha256'],explicit_sha256=e['sha256']))
    if complete!=dict(commands=25,archives_verified=10,payload_compares=5,search_commands=6,all_byte_exact=True):
        raise ValueError('completion claims differ')
    fixed=next(t for t in table if t['variant']=='fixed')
    for name in ['warm','cold']:
        for mode in ['seed','explicit']:
            r=by_label[name+'-'+mode]['result']
            if r['baseline_bytes']!=fixed['bytes']:
                raise ValueError('searched default differs from independent fixed archive')
    warm,cold=[by_label[n+'-seed']['result'] for n in ['warm','cold']]
    return dict(status='independently audited from raw command JSON and artifact ledger',
        commands=25,archives_verified=10,payload_compares=5,search_audits=audits,table=table,
        warm_cold_same_start=warm['start_plan']==cold['start_plan'],
        warm_cold_same_candidates=warm['evaluated']==cold['evaluated'],
        peak_memory_bytes=max(r['result'].get('peak_memory_bytes',0) for r in raw),
        disk_observed_peak_bytes=max(max(r['disk_before'],r['disk_after']) for r in raw),
        limitations='one dev observation; 29 concrete plans; single merged update; no holdout/significance')


if __name__=='__main__':
    result=analyze(Path(sys.argv[1]),Path(sys.argv[2]))
    Path(sys.argv[3]).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
