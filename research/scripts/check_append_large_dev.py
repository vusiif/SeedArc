# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the disk-bounded scheduler with independent-history reference artifacts."""
import argparse
import json
from pathlib import Path

import append_history_r2 as reference
import append_large_dev as large


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    args=parser.parse_args()
    args.work.mkdir(exist_ok=False)
    source=args.work/'input'
    source.mkdir()
    manifest=args.work/'manifest.tsv'
    lines=[]
    for stage,batch in enumerate(reference.BATCHES):
        for i in range(8):
            folder=['src','docs',f'new-{stage}'][i%3]
            rel=f'{folder}/文件-{stage}-{i}.txt'
            path=source/rel
            path.parent.mkdir(exist_ok=True)
            data=(f'phase {stage}, item {i}: alpha beta gamma key=value function()\n'*60).encode()
            path.write_bytes(data)
            lines.append(f'{rel}\t{len(data)}\t{reference.sha(path)}\t{folder}\t{batch}\n')
    manifest.write_text(''.join(lines),encoding='utf-8')
    reference.run(args.tool,source,manifest,args.work/'reference',budget_multiple=4)
    expected={(r['branch'],r['stage']):r for r in map(json.loads,
              (args.work/'reference/histories.jsonl').read_text().splitlines())}
    prior=None
    for iteration in range(2):
        out=args.work/f'out-{iteration}'
        scratch=args.work/f'work-{iteration}'
        result=large.run(args.tool,source,manifest,scratch,out,args.work,4)
        histories=list(map(json.loads,(out/'histories.jsonl').read_text().splitlines()))
        assert len(histories)==34 and result['artifacts']==34
        for row in histories:
            old=expected[row['branch'],row['stage']]
            for key in ['plan','bytes','sha256','generations','byte_exact']:
                assert row[key]==old[key],(key,row,old)
            if row['stage'] and not row['branch'].startswith('R-'):
                assert row['parent_sha256']==expected[row['branch'],row['stage']-1]['sha256']
            else:
                assert row['parent_sha256'] is None
        if prior is not None:
            assert histories==prior
        prior=histories
        assert not list(scratch.iterdir()),'branch files not reclaimed'
        for row in map(json.loads,(out/'raw.jsonl').read_text().splitlines()):
            if row['branch'].startswith('R-'):
                assert row['parent'] is None
    for name,text in [('bad-hash',''.join(lines).replace(lines[0].split('\t')[2],'0'*64,1)),
                      ('duplicate',''.join(lines)+lines[0]),
                      ('over-limit','x\t536870913\t'+'0'*64+'\tx\tinitial\n')]:
        bad=args.work/(name+'.tsv')
        bad.write_text(text,encoding='utf-8')
        try:
            large.rows(source,bad)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid manifest accepted: '+name)
    summary=dict(scope='functional scheduler acceptance',passes=2,artifacts_per_pass=34,
        identical_to_reference=True,repeatable=True,chinese_paths=True,independent_parents=True,
        invalid_manifests_rejected=True,branch_files_reclaimed=True,tool_sha256=reference.sha(args.tool))
    reference.write_json(args.work/'acceptance.json',summary)
    print(json.dumps(summary))


if __name__=='__main__':
    main()
