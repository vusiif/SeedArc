# SPDX-License-Identifier: GPL-3.0-or-later
"""Independently check completed large dev histories and report observed costs."""
import argparse
import json
from pathlib import Path


def analyze(root):
    config=json.loads((root/'config.json').read_text())
    complete=json.loads((root/('aggregation.json' if (root/'aggregation.json').exists() else 'complete.json')).read_text())
    raw=list(map(json.loads,(root/'raw.jsonl').read_text().splitlines()))
    histories=list(map(json.loads,(root/'histories.jsonl').read_text().splitlines()))
    if len(raw)!=complete['commands'] or len(histories)!=complete['artifacts']:
        raise ValueError('completion counts differ from raw records')
    if any(r['returncode']!=0 for r in raw):
        raise ValueError('nonzero completed tool command')
    keyed={(r['branch'],r['stage']):r for r in histories}
    if len(keyed)!=len(histories):
        raise ValueError('duplicate branch/stage history')
    expected_keys={(b,s) for b in complete['branches'] for s in range(4)
                   if not (s==0 and b.startswith('R-'))}
    if set(keyed)!=expected_keys:
        raise ValueError('missing or unexpected history stage')
    batches=['initial','append1','append2','append3']
    manifest=[line.split('\t') for line in (root/'manifest.tsv').read_text().splitlines()]
    for row in histories:
        branch,stage=row['branch'],row['stage']
        cold=branch.startswith('R-')
        parent=keyed[branch,stage-1] if stage and not cold else None
        if row['parent_sha256']!=(parent['sha256'] if parent else None):
            raise ValueError('incorrect independent parent')
        keep=stage>0 and branch in ['A','B-new','D-nearest','S-segment']
        if not row['byte_exact'] or row['old_is_prefix']!=keep:
            raise ValueError('missing byte-exact/prefix contract')
        expected=stage+1 if branch in ['A','B-new','D-nearest','S-segment'] else 1
        if row['generations']!=expected:
            raise ValueError('generation contract mismatch')
        calls=[r for r in raw if r['branch']==branch and r['stage']==stage]
        verify=[r for r in calls if r['command']=='verify']
        if len(verify)!=1 or not verify[0]['result'].get('byte_exact'):
            raise ValueError('missing independent verification')
        writing=[r for r in calls if r['command'] in ['pack','append']]
        if len(writing)!=1 or writing[0]['result']['container_bytes']!=row['bytes']:
            raise ValueError('archive size differs from operation output')
        for search in [r for r in calls if r['command']=='search']:
            result=search['result']
            if result['restart']!=cold:
                raise ValueError('cold/warm policy mismatch')
            if parent and result['start_plan']!=parent['plan']:
                raise ValueError('search did not use its own prior plan')
            if result['best_plan']!=row['plan']:
                raise ValueError('archive did not use search output plan')
            size=sum(int(r[1]) for r in manifest if (r[4]==batches[stage] if branch=='B-new'
                     else batches.index(r[4])<=stage))
            if (result['budget_bytes']!=size*config['budget_multiple'] or
                    result['budget_spent_bytes']>result['budget_bytes'] or
                    result['best_bytes']>result['start_bytes']):
                raise ValueError('search budget or best-so-far contract mismatch')
    table=[]
    for branch in complete['branches']:
        final=keyed[branch,3]
        calls=[r for r in raw if r['branch']==branch]
        appended=[r for r in calls if r['stage']>0]
        op=[r for r in appended if r['command'] in ['pack','append']]
        search=[r for r in appended if r['command']=='search']
        last_write=next(r for r in op if r['stage']==3)
        parts=last_write['result']['parts']
        physical=['header','generation_records','dictionaries','data','redundancy_records',
                  'stale_trailers','trailer','footer']
        if sum(parts[k] for k in physical)!=final['bytes']:
            raise ValueError('physical archive parts do not reconcile')
        table.append(dict(branch=branch,final_bytes=final['bytes'],final_plan=final['plan'],
            input_bytes=config['bytes'],ratio=final['bytes']/config['bytes'],
            initial_write_ms=sum(r['process_wall_ms'] for r in calls if r['stage']==0 and r['command']=='pack'),
            append_search_ms=sum(r['process_wall_ms'] for r in search),
            append_write_ms=sum(r['process_wall_ms'] for r in op),
            append_search_plus_write_ms=sum(r['process_wall_ms'] for r in search+op),
            independent_verify_ms=sum(r['process_wall_ms'] for r in appended if r['command']=='verify'),
            peak_tool_rss_bytes=max(r['result']['peak_memory_bytes'] for r in calls),
            final_parts=parts))
    solid=next(r['final_bytes'] for r in table if r['branch']=='S-regroup')
    for row in table:
        row['size_vs_solid_regroup_percent']=100*(row['final_bytes']/solid-1)
    a=next(r for r in table if r['branch']=='A')
    physical=['header','generation_records','dictionaries','data','redundancy_records',
              'stale_trailers','trailer','footer']
    deltas={row['branch']:{k:row['final_parts'][k]-a['final_parts'][k] for k in physical}
            for row in table if row['branch']!='A'}
    searches=[dict(branch=r['branch'],stage=r['stage'],process_wall_ms=r['process_wall_ms'],
        **{k:r['result'].get(k) for k in ['method','start_plan','best_plan','start_bytes','best_bytes',
        'budget_bytes','budget_spent_bytes','plan_evaluations','group_evaluations','group_cache_hits',
        'stop','trajectory']}) for r in raw if r['command']=='search']
    return dict(scope='single-run same-source dev costs; no Seed advantage conclusion',
        validated=dict(commands=len(raw),artifacts=len(histories),byte_exact=True,independent_histories=True),
        costs='sum subprocess wall time for stages 1..3; includes tool internal verification, excludes staging, prewarming and external verify (separately reported)',
        provenance=complete.get('sources'),
        limitations=['One PRNG seed and one repetition; no statistical estimate.',
                     'For an aggregate, the interruption and task-local monitor change must be disclosed; no uninterrupted-run claim.',
                     'Existing separable space; bounded searches do not establish global optimum or infeasible exhaustive cost.',
                     'No B-mix, cross-type drift, single large-file case or holdout evaluation.',
                     'Archives were checked and hashed before reclamation; reproduce from fixed inputs to regenerate.'],
        branches=table,physical_parts_delta_vs_A=deltas,searches=searches,
        peak_observed_task_disk=complete['peak_observed_task_disk'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--markdown',type=Path)
    args=parser.parse_args()
    if args.output.exists():
        raise FileExistsError('do not overwrite an existing analysis')
    if args.markdown and args.markdown.exists():
        raise FileExistsError('do not overwrite an existing Markdown report')
    result=analyze(args.results)
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if args.markdown:
        lines=['# 大目录 dev 成本观测', '',
            '512 MiB Linux 6.12 源码，4× 搜索预算，seed3，单遍；中断与续测来自两个任务，非正式留出评估。', '',
            '时间为三批新增阶段的工具进程墙钟合计，含内部校验；不含初始打包、输入准备、预热和单独 verify。跨任务监控变化可能影响时间，不能作显著性判断。MB 为十进制。', '',
            '| 路径 | 最终 MB | 相对固实重压 | 搜索 s | 写包 / 追加 s | 合计 s | 独立校验 s | 峰值 RSS MiB |',
            '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
        for r in result['branches']:
            lines.append(f"| {r['branch']} | {r['final_bytes']/1e6:.3f} | {r['size_vs_solid_regroup_percent']:+.2f}% | "
                         f"{r['append_search_ms']/1000:.2f} | {r['append_write_ms']/1000:.2f} | "
                         f"{r['append_search_plus_write_ms']/1000:.2f} | {r['independent_verify_ms']/1000:.2f} | "
                         f"{r['peak_tool_rss_bytes']/1024**2:.1f} |")
        lines+=['', '搜索仅评估现有可分空间；不代表全局最优或难搜索空间。本轮不含 B-mix、内容漂移或单大文件；完整预算、起点、轨迹与物理字节分解见 analysis.json。', '']
        args.markdown.write_text('\n'.join(lines),encoding='utf-8')
    for row in result['branches']:
        print(row['branch'],row['final_bytes'],round(row['size_vs_solid_regroup_percent'],2),
              round(row['append_search_plus_write_ms']/1000,3))


if __name__=='__main__':
    main()
