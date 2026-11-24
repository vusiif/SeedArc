# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive a labeled analysis input from interrupted and continuation evidence.

Never write a result.json or completion marker into either source task. The
aggregate is explicitly derived and keeps each command's original task/index.
"""
import argparse
import json
from pathlib import Path
import shutil

from append_history_r2 import sha, write_json


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interrupted',type=Path)
    parser.add_argument('continuation',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    first,second=args.interrupted,args.continuation
    configs=[json.loads((p/'config.json').read_text()) for p in [first,second]]
    for key in ['tool_sha256','runner_sha256','manifest_sha256','bytes','files','initial_plan',
                'budget_multiple','prng_seed','platform','affinity','max_bytes','disk_gate']:
        if configs[0][key]!=configs[1][key]:
            raise ValueError('source configurations differ: '+key)
    if sha(first/'manifest.tsv')!=sha(second/'manifest.tsv'):
        raise ValueError('source input manifests differ')
    complete=json.loads((second/'complete.json').read_text())
    tasks=['20261124T051435Z-cef36683','20261124T054749Z-aef206e4']
    raw,histories=[],[]
    branches=[]
    sources=[]
    for i,root in enumerate([first,second]):
        rows=list(map(json.loads,(root/'histories.jsonl').read_text().splitlines()))
        own=list(dict.fromkeys(r['branch'] for r in rows))
        for branch in own:
            stages={r['stage'] for r in rows if r['branch']==branch}
            if stages!=({1,2,3} if branch.startswith('R-') else {0,1,2,3}):
                raise ValueError('source has an incomplete branch')
        if set(own)&set(branches):
            raise ValueError('continuation repeats an existing branch')
        branches+=own
        calls=list(map(json.loads,(root/'raw.jsonl').read_text().splitlines()))
        if any(r['returncode']!=0 for r in calls) or any(r['branch'] not in own for r in calls):
            raise ValueError('source has failed or unverified partial commands')
        for row in calls:
            raw.append(dict(row,source_task_id=tasks[i],source_index=row['index'],index=len(raw)))
        histories.extend(dict(row,source_task_id=tasks[i]) for row in rows)
        sources.append(dict(task_id=tasks[i],raw_sha256=sha(root/'raw.jsonl'),
                            histories_sha256=sha(root/'histories.jsonl'),branches=own))
    if set(branches)!=set(configs[0]['branches']) or len(raw)!=83 or len(histories)!=34:
        raise ValueError('aggregate branch/command/artifact count mismatch')
    args.output.mkdir(exist_ok=False)
    write_json(args.output/'config.json',dict(configs[0],scope='derived interrupted + continuation dev observation'))
    shutil.copyfile(first/'manifest.tsv',args.output/'manifest.tsv')
    for name,rows in [('raw.jsonl',raw),('histories.jsonl',histories)]:
        (args.output/name).write_text(''.join(json.dumps(r)+'\n' for r in rows))
    write_json(args.output/'aggregation.json',dict(scope='derived analysis input, not task completion',
        interrupted_task_has_normal_result=False,sources=sources,commands=len(raw),artifacts=len(histories),
        branches=branches,peak_observed_task_disk=max(r['disk_after'] for r in raw),
        caveat='Monitor implementation changed after interruption; timing is exploratory and not a statistical comparison.'))
    print(json.dumps(dict(commands=len(raw),artifacts=len(histories),sources=sources)))


if __name__=='__main__':
    main()
