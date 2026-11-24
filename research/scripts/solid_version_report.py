# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit the mixed/version matrix without inferring benefits from representation alone."""
import json
from pathlib import Path
import sys
from append_history_r2 import sha
from solid_search_report import audit_search


def analyze(root,m0,m1,delta=None):
    config=json.loads((root/'config.json').read_text())
    raw=[json.loads(s) for s in (root/'raw.jsonl').read_text().splitlines()]
    archives=json.loads((root/'archives.json').read_text())
    complete=json.loads((root/'complete.json').read_text())
    if config['v0_manifest_sha256']!=sha(m0) or config['v1_manifest_sha256']!=sha(m1):
        raise ValueError('manifest mismatch')
    manifests=[[line.split('\t') for line in p.read_text().splitlines()] for p in [m0,m1]]
    totals=[sum(int(r[1]) for r in rows) for rows in manifests]
    if config['bytes']!=totals or config['files']!=len(manifests[0]) or len(manifests[0])!=len(manifests[1]):
        raise ValueError('input byte/count configuration mismatch')
    if (len(raw)!=25 or len(archives)!=10 or [r['index'] for r in raw]!=list(range(25))
        or any(r['returncode'] for r in raw)):
        raise ValueError('incomplete or failed matrix')
    searches=[r for r in raw if r['argv'][0]=='solid-search']
    verifies=[r for r in raw if r['argv'][0]=='verify']
    compares=[r for r in raw if r['argv'][0]=='payload-compare']
    if len(searches)!=6 or len(verifies)!=10 or len(compares)!=5:
        raise ValueError('wrong coverage')
    if (any(not r['result']['byte_exact'] or r['result']['generations']!=1 for r in verifies)
        or any(not r['result']['equal'] for r in compares)):
        raise ValueError('snapshot verification failed')
    by_label={r['label']:r for r in raw}
    audits={r['label']:audit_search(r['result']) for r in searches}
    for item in searches:
        r=item['result']
        initial_call=item['label'].startswith('initial-')
        multiple=29 if initial_call else 8
        if (r['policy']!=('enumerate' if initial_call else 'near') or r['prng_seed']!=3
            or r['budget_bytes']!=r['input_bytes']*multiple):
            raise ValueError('search operator or pre-registered budget changed')
    table=[]
    for variant in ['initial','fixed','reuse','warm','cold']:
        pair=[next(a for a in archives if a['label']==variant+'-'+m) for m in ['seed','explicit']]
        s,e=pair
        if s['bytes']!=e['bytes'] or s['result']['parts']!=e['result']['parts']:
            raise ValueError('representation changed archive sizes/parts')
        for a in pair:
            result=a['result']
            if (a['bytes']!=result['container_bytes'] or result['container_revision']!=5
                or result['strategy_revision']!=2 or result['generations']!=1
                or result['files']!=len(manifests[0])
                or sum(v for k,v in result['parts'].items() if k!='group_metadata')!=a['bytes']):
                raise ValueError('archive byte accounting failed')
        if variant in ['initial','warm','cold']:
            for field in ['start_plan','best_plan','evaluated','budget_spent_bytes']:
                if s['result'][field]!=e['result'][field]:
                    raise ValueError('same-operator representation difference')
            count=29 if variant=='initial' else 8
            if (s['result']['evaluations']!=count or s['result']['space_exhausted']!=(count==29)
                or s['result']['input_bytes']!=totals[0 if variant=='initial' else 1]):
                raise ValueError('wrong planned coverage')
        for mode,a in zip(['seed','explicit'],pair):
            prior=by_label['initial-'+mode]['result']['best_plan']
            if variant=='warm' and a['result']['start_plan']!=prior:
                raise ValueError('warm start was not inherited')
            if variant=='reuse':
                r=a['result']
                if r['k']!=prior['groups'] or r['rule']!=['dir1','dir2','ext','size'][prior['rule']]:
                    raise ValueError('zero-search reuse changed concrete plan')
            if variant=='cold' and (a['result']['start_plan']['rule'],a['result']['start_plan']['groups'])!=(0,1):
                raise ValueError('cold did not start at default')
        table.append(dict(variant=variant,bytes=s['bytes'],payload_bytes=s['result']['parts']['data'],
            metadata_bytes=s['bytes']-s['result']['parts']['data'],
            seed_tool_seconds=s['result']['call_wall_ms']/1000,
            explicit_tool_seconds=e['result']['call_wall_ms']/1000,
            seed_external_verify_seconds=by_label[s['label']+'-verify']['process_wall_ms']/1000,
            explicit_external_verify_seconds=by_label[e['label']+'-verify']['process_wall_ms']/1000,
            plan=s['result'].get('best_plan'),seed_sha256=s['sha256'],explicit_sha256=e['sha256']))
    fixed=next(t for t in table if t['variant']=='fixed')
    for name in ['warm','cold']:
        if by_label[name+'-seed']['result']['baseline_bytes']!=fixed['bytes']:
            raise ValueError('strong fallback differs from independent fixed pack')
    initial=by_label['initial-seed']['result']
    prefix=[]
    for count in [4,8,29]:
        trials=initial['trials'][:count]
        best=min(trials,key=lambda t:(t['bytes'],t['plan']['rule'],t['plan']['groups']))
        prefix.append(dict(evaluations=count,bytes=best['bytes'],plan=best['plan'],
            spent_bytes=trials[-1]['spent_bytes'],observed_elapsed_ms=trials[-1]['elapsed_wall_ms'],
            scope='prefix of one exhaustive enumeration; not an independently timed invocation'))
    warm,cold=[by_label[n+'-seed']['result'] for n in ['warm','cold']]
    goal=cold['best_bytes']
    reached=next((t for t in warm['trials'] if t['bytes']<=goal),None)
    cold_reached=next(t for t in cold['trials'] if t['bytes']<=goal)
    def publishable(result,hit):
        if hit is None:
            return None
        fallback=next(t for t in result['trials'] if (t['plan']['rule'],t['plan']['groups'])==(0,1))
        count=max(hit['evaluation'],fallback['evaluation'])
        return dict(evaluations=count,spent_bytes=count*result['accounting_unit_bytes'],
                    observed_elapsed_ms=result['trials'][count-1]['elapsed_wall_ms'],
                    limitation='Observed prefix after mandatory default; excludes final publication, not an independently stopped run')
    result=dict(status='audited from complete raw command and artifact ledgers',commands=25,
        archives_verified=10,payload_compares=5,search_audits=audits,table=table,
        initial_prefix_reference=prefix,initial_nondefault=(initial['best_plan']['rule'],initial['best_plan']['groups'])!=(0,1),
        warm_cold_same_start=warm['start_plan']==cold['start_plan'],warm_cold_same_candidates=warm['evaluated']==cold['evaluated'],
        warm_evaluations_to_cold8_quality=reached['evaluation'] if reached else None,
        cold_evaluations_to_cold8_quality=cold_reached['evaluation'],
        warm_observed_ms_to_cold8_quality=reached['elapsed_wall_ms'] if reached else None,
        cold_observed_ms_to_cold8_quality=cold_reached['elapsed_wall_ms'],
        warm_publishable_prefix_to_cold8_quality=publishable(warm,reached),
        cold_publishable_prefix_to_cold8_quality=publishable(cold,cold_reached),
        initial_gain_from_default_bytes=initial['baseline_bytes']-initial['best_bytes'],
        update_warm_gain_from_default_bytes=warm['baseline_bytes']-warm['best_bytes'],
        update_cold_gain_from_default_bytes=cold['baseline_bytes']-cold['best_bytes'],
        peak_memory_bytes=max(r['result'].get('peak_memory_bytes',0) for r in raw),
        disk_observed_peak_bytes=max(max(r['disk_before'],r['disk_after']) for r in raw),
        limitations='One dev observation; v0 exhaustive29, v1 near8. Full snapshot encoding, not incremental archive update.')
    expected=dict(commands=25,archives_verified=10,payload_compares=5,search_commands=6,all_byte_exact=True,v0_unchanged=True)
    if complete!=expected:
        raise ValueError('completion record mismatch')
    if delta:
        change=json.loads(delta.read_text())
        old={line.split('\t')[0]:line.split('\t') for line in m0.read_text().splitlines()}
        new={line.split('\t')[0]:line.split('\t') for line in m1.read_text().splitlines()}
        changed={p for p in old if old[p][1:3]!=new.get(p,[])[1:3]}
        if set(old)!=set(new) or changed!={'linux/'+r['path'] for r in change['changes']}:
            raise ValueError('version manifest delta differs from verified patch')
        if (change['changed_files']!=len(changed) or change['unchanged_files']!=len(old)-len(changed)
            or any(change[k] for k in ['additions','deletions','renames'])):
            raise ValueError('version scope/count changed')
        if any(old[p][3:]!=new[p][3:] for p in old):
            raise ValueError('version changed original source/cohort assignments')
        for r in change['changes']:
            p='linux/'+r['path']
            if (old[p][2]!=r['old_sha256'] or new[p][2]!=r['new_sha256']
                or int(old[p][1])!=r['old_bytes'] or int(new[p][1])!=r['new_bytes']):
                raise ValueError('version content hash differs')
        result['version_changed_files']=len(changed)
        result['version_input_bytes_delta']=sum(int(r[1]) for r in new.values())-sum(int(r[1]) for r in old.values())
    return result


if __name__=='__main__':
    result=analyze(Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]),Path(sys.argv[4]))
    Path(sys.argv[5]).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
