# SPDX-License-Identifier: GPL-3.0-or-later
"""Run dev-only independent histories, retaining metrics instead of all archives.

One branch at a time; only its current and next archive coexist. This is a new
bounded runner, not a removal of the 8 MiB functional acceptance runner's gate.
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import time

from append_history_r2 import BATCHES, sha, prefix_matches, write_json

MAX_BYTES = 512 * 1024**2
DISK_GATE = 1900 * 1024**2
BRANCHES = {'A':'seed', 'B-new':'seed', 'C':'seed', 'C-coord':'explicit',
            'D-nearest':'explicit', 'S-segment':'solid', 'S-regroup':'solid',
            'R-seed':'seed', 'R-coord':'explicit'}


def rows(root, manifest):
    selected, paths, hashes, total = [], set(), set(), 0
    for line in manifest.read_text(encoding='utf-8').splitlines():
        rel, size, digest, group, batch = line.split('\t')
        path = PurePosixPath(rel)
        if (path.is_absolute() or path.as_posix()!=rel or '..' in path.parts or ':' in rel
                or '\\' in rel or rel in paths or batch not in BATCHES):
            raise ValueError('unsafe or duplicate manifest path')
        size = int(size)
        if size <= 0 or len(digest)!=64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('invalid size/hash')
        total += size
        if total > MAX_BYTES or len(selected) >= 100000 or digest in hashes:
            raise ValueError('dev size/count/dedup bound exceeded')
        current = root
        for part in path.parts:
            current /= part
            if current.is_symlink() or (hasattr(current,'is_junction') and current.is_junction()):
                raise ValueError('linked source is not accepted')
        if not current.is_file() or current.stat().st_size!=size or sha(current)!=digest:
            raise ValueError('source hash/size differs: '+rel)
        selected.append(dict(path=rel,size=size,sha256=digest,group=group,batch=batch))
        paths.add(rel)
        hashes.add(digest)
    if any(not any(r['batch']==b for r in selected) for b in BATCHES):
        raise ValueError('all four batches must be nonempty')
    return selected


def disk_bytes(root):
    return sum(p.lstat().st_size for p in root.rglob('*') if p.is_file() and not p.is_symlink())


def remove_owned(path, owner):
    path, owner = Path(path), Path(owner).resolve()
    if path.is_symlink() or path.resolve().parent != owner:
        raise ValueError('unexpected owned cleanup root')
    if any(p.is_symlink() for p in path.rglob('*')):
        raise ValueError('link in owned cleanup tree')
    shutil.rmtree(path)


def run(tool, root, manifest, work, output, task_root, multiple=4, prng=3,
        initial='0x148', branches=None):
    tool, root, manifest = tool.resolve(strict=True), root.resolve(strict=True), manifest.resolve(strict=True)
    selected = rows(root, manifest)
    work.mkdir(exist_ok=False)
    output.mkdir(exist_ok=False)
    logs=output/'commands'
    logs.mkdir()
    affinity={min(os.sched_getaffinity(0))} if hasattr(os,'sched_getaffinity') else None
    branches=list(branches or BRANCHES)
    if not branches or len(set(branches))!=len(branches) or any(b not in BRANCHES for b in branches):
        raise ValueError('invalid branches')
    write_json(output/'config.json',dict(scope='dev exploratory; one repetition, not holdout or significance',
        tool_sha256=sha(tool), runner_sha256=sha(Path(__file__)), manifest_sha256=sha(manifest),
        bytes=sum(r['size'] for r in selected),files=len(selected),initial_plan=initial,
        budget_multiple=multiple,prng_seed=prng,branches=branches,platform=platform.platform(),
        affinity=sorted(affinity) if affinity else None, max_bytes=MAX_BYTES,disk_gate=DISK_GATE,
        cache='read current input before each tool call; no global cache dropping',
        retention='hashes and complete JSON/logs; discard verified superseded/final archives',
        space='existing separable revision; no hard-space or Seed advantage assumed'))
    shutil.copyfile(manifest,output/'manifest.tsv')
    index, histories, disk_peak=0, [], 0

    def call(command,args,branch,stage,tree,parent=None):
        nonlocal index,disk_peak
        # Warm only this call's input. Never alter OS-wide cache state.
        for path in sorted(tree.rglob('*')):
            if path.is_file():
                with path.open('rb') as stream:
                    while stream.read(1024**2):
                        pass
        used=disk_bytes(task_root)
        if used>=DISK_GATE:
            raise RuntimeError('task disk headroom exhausted; retain evidence')
        # Before output-producing calls reserve a conservative raw output plus metadata.
        if command in ('pack','append'):
            size=sum(r['size'] for r in selected if BATCHES.index(r['batch'])<=stage)
            reserve=size+len(selected)*1024+16*1024**2
            if used+reserve>=2*1024**3:
                raise RuntimeError('insufficient worst-case archive disk budget')
        def fixed_cpu():
            os.sched_setaffinity(0,affinity)
        start=time.perf_counter()
        argv=[str(tool),command,*map(str,args)]
        proc=subprocess.run(argv,capture_output=True,text=True,encoding='utf-8',timeout=1800,
                            preexec_fn=fixed_cpu if affinity else None)
        elapsed=(time.perf_counter()-start)*1000
        name=f'{index:03d}-{branch}-{stage}-{command}'
        (logs/(name+'.stdout')).write_text(proc.stdout,encoding='utf-8')
        (logs/(name+'.stderr')).write_text(proc.stderr,encoding='utf-8')
        after=disk_bytes(task_root)
        disk_peak=max(disk_peak,used,after)
        rec=dict(index=index,branch=branch,stage=stage,command=command,argv=argv,
                 parent=parent,returncode=proc.returncode,process_wall_ms=elapsed,
                 disk_before=used,disk_after=after)
        index+=1
        if proc.returncode==0:
            rec['result']=json.loads(proc.stdout)
        with (output/'raw.jsonl').open('a',encoding='utf-8') as stream:
            stream.write(json.dumps(rec)+'\n')
        print(json.dumps(dict(index=rec['index'],branch=branch,stage=stage,command=command,
                              returncode=proc.returncode,process_wall_ms=elapsed)),flush=True)
        if proc.returncode or after>=DISK_GATE:
            raise RuntimeError('tool failed or disk gate reached; artifacts retained')
        return rec['result']

    for branch in branches:
        branch_work=work/branch
        branch_work.mkdir()
        cumulative=branch_work/'cumulative'
        cumulative.mkdir()
        parent=None
        for stage,batch in enumerate(BATCHES):
            added=branch_work/'added'
            added.mkdir()
            for row in selected:
                if row['batch']==batch:
                    dest=added/row['path']
                    dest.parent.mkdir(parents=True,exist_ok=True)
                    os.link(root/row['path'],dest)
            rebuild=not stage or branch in ('C','C-coord','R-seed','R-coord')
            def merge_added():
                for row in selected:
                    if row['batch']==batch:
                        dest=cumulative/row['path']
                        dest.parent.mkdir(parents=True,exist_ok=True)
                        (added/row['path']).rename(dest)
                remove_owned(added,branch_work)
            if rebuild:
                merge_added()
            if stage==0 and branch.startswith('R-'):
                continue  # Cold controls begin at the first append, as in the reference runner.
            context=None if branch.startswith('R-') else parent
            plan=initial if stage==0 else 'default' if branch.startswith('R-') else parent['plan']
            if stage and branch in ('B-new','C','C-coord','R-seed','R-coord'):
                tree=added if branch=='B-new' else cumulative
                size=sum(r['size'] for r in selected if (r['batch']==batch if branch=='B-new'
                         else BATCHES.index(r['batch'])<=stage))
                method='coord' if branch in ('C-coord','R-coord') else 'seed'
                policy='restart' if branch.startswith('R-') else 'local'
                searched=call('search',[method,plan,tree,size*multiple,prng,policy],branch,stage,tree,context)
                if (plan!='default' and searched['start_plan']!=plan) or searched['restart']!=(policy=='restart'):
                    raise AssertionError('search start or policy differs from branch semantics')
                plan=searched['best_plan']
            archive=branch_work/f'{stage}.sadx'
            if rebuild:
                result=call('pack',[BRANCHES[branch],plan,cumulative,archive,'off'],branch,stage,cumulative,context)
            else:
                args=[branch_work/parent['artifact'],added,archive]
                args+=['--plan',plan] if branch=='B-new' else ['regroup' if branch=='S-regroup' else 'segment']
                result=call('append',args,branch,stage,added,parent)
                merge_added()
            checked=call('verify',[archive,cumulative],branch,stage,cumulative,context)
            expected_gens=stage+1 if branch in ('A','B-new','D-nearest','S-segment') else 1
            if not checked.get('byte_exact') or checked['generations']!=expected_gens:
                raise AssertionError('roundtrip or generation contract failed')
            keeps_prefix=bool(stage and branch in ('A','B-new','D-nearest','S-segment'))
            if parent:
                old=branch_work/parent['artifact']
                if sha(old)!=parent['sha256']:
                    raise AssertionError('old archive modified')
                if keeps_prefix and (not result.get('old_is_prefix') or not prefix_matches(old,archive)):
                    raise AssertionError('old archive not exact prefix')
            current=dict(branch=branch,stage=stage,plan=plan,artifact=archive.name,
                bytes=archive.stat().st_size,sha256=sha(archive),parent_sha256=context['sha256'] if context else None,
                parent_plan=context['plan'] if context else None,generations=expected_gens,
                old_is_prefix=keeps_prefix,byte_exact=True)
            histories.append(current)
            with (output/'histories.jsonl').open('a') as stream:
                stream.write(json.dumps(current)+'\n')
            if parent:
                (branch_work/parent['artifact']).unlink()
            parent=current
        remove_owned(branch_work,work)
    result=dict(scope='dev only; not frozen evaluation',commands=index,artifacts=len(histories),
                branches=branches,stages=4,byte_exact=True,peak_observed_task_disk=disk_peak)
    write_json(output/'complete.json',result)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['tool','input','manifest','work','output','task-root']:
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--budget-multiple',type=int,default=4)
    parser.add_argument('--prng-seed',type=int,default=3)
    args=parser.parse_args()
    if args.budget_multiple<1 or args.budget_multiple>32:
        parser.error('budget must be in 1..32 input multiples')
    print(json.dumps(run(args.tool,args.input,args.manifest,args.work,args.output,args.task_root,
                         args.budget_multiple,args.prng_seed)))
