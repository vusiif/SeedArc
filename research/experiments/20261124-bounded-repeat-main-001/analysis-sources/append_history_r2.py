# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate independent append histories against the existing research v4 tool.

This is a functional acceptance runner, not the large-scale performance experiment.
Inputs are capped at 8 MiB intentionally; the r2 resource/complexity gates remain open.
The original pilot and frozen G5 tool are not modified.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import sys

MAX_ACCEPTANCE_BYTES = 8 * 1024 * 1024
BATCHES = ('initial', 'append1', 'append2', 'append3')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def load_manifest(root, manifest):
    selected, seen = [], set()
    total = 0
    for line in manifest.read_text(encoding='utf-8').splitlines():
        rel, size_text, digest, group, batch = line.split('\t')
        path = PurePosixPath(rel)
        if (path.is_absolute() or '..' in path.parts or '\\' in rel or ':' in rel
                or path.as_posix() != rel or rel in seen or batch not in BATCHES):
            raise ValueError('unsafe, duplicate or noncanonical manifest path/batch')
        size = int(size_text)
        if size < 0 or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('invalid size or SHA256')
        total += size
        if total > MAX_ACCEPTANCE_BYTES:
            raise ValueError('functional acceptance limit is 8 MiB; large-scale gates are not met')
        current = root
        for part in path.parts:
            current /= part
            if current.is_symlink() or (hasattr(current, 'is_junction') and current.is_junction()):
                raise ValueError('linked input is not allowed')
        if not current.is_file() or current.stat().st_size != size or sha(current) != digest:
            raise ValueError(f'input differs from manifest: {rel}')
        seen.add(rel)
        selected.append(dict(path=rel, size=size, sha256=digest, group=group, batch=batch))
    if not selected or any(not any(r['batch'] == batch for r in selected) for batch in BATCHES):
        raise ValueError('all four batches must be nonempty')
    return selected


def prefix_matches(parent, child):
    with parent.open('rb') as old, child.open('rb') as new:
        while block := old.read(1024 * 1024):
            if new.read(len(block)) != block:
                return False
    return True


def run(tool, root, manifest, output, initial_plan='0x148', budget_multiple=8, prng=3):
    root = root.resolve(strict=True)
    tool = tool.resolve(strict=True)
    manifest = manifest.resolve(strict=True)
    output = output.absolute()
    # Validate before creating outputs; do not reuse or erase any existing directory.
    selected = load_manifest(root, manifest)
    output.mkdir(parents=False, exist_ok=False)
    artifacts = output / 'artifacts'
    artifacts.mkdir()
    logs = output / 'commands'
    logs.mkdir()
    cumulative = output / 'cumulative'
    cumulative.mkdir()
    shutil.copyfile(manifest, output / 'manifest.tsv')
    shutil.copyfile(Path(__file__), output / 'runner.py')
    records, states = [], {}
    branches = {'A': 'seed', 'B-new': 'seed', 'C': 'seed', 'C-coord': 'explicit',
                'D-nearest': 'explicit', 'S-segment': 'solid', 'S-regroup': 'solid'}
    write_json(output / 'config.json', dict(scope='functional acceptance only',
               large_input_gate='not met', coupled_space_gate='not met',
               tool_sha256=sha(tool), runner_sha256=sha(Path(__file__)),
               manifest_sha256=sha(manifest), initial_plan=initial_plan,
               budget_multiple=budget_multiple, prng_seed=prng,
               platform=platform.platform(), max_bytes=MAX_ACCEPTANCE_BYTES,
               B_new='own parent, local search on new batch, fresh dictionaries only',
               C='own parent plan, local search on cumulative input, full repack'))

    def call(command, args, branch, stage, parent=None):
        argv = [str(tool), command, *[str(a) for a in args]]
        proc = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8', timeout=300)
        index = len(records)
        name = f'{index:03d}-{stage}-{branch}-{command}'
        (logs / (name + '.stdout')).write_text(proc.stdout, encoding='utf-8')
        (logs / (name + '.stderr')).write_text(proc.stderr, encoding='utf-8')
        rec = dict(index=index, stage=stage, branch=branch, command=command, argv=argv,
                   parent_artifact=parent['artifact'] if parent else None,
                   parent_plan=parent['plan'] if parent else None, returncode=proc.returncode)
        if proc.returncode == 0:
            rec['result'] = json.loads(proc.stdout)
        records.append(rec)
        with (output / 'raw.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(rec, ensure_ascii=False) + '\n')
        if proc.returncode:
            raise RuntimeError(f'{name} failed: {proc.returncode}; logs retained')
        return rec['result']

    def search(branch, stage, start, tree, policy, parent, size):
        method = 'coord' if branch in ('C-coord', 'R-coord') else 'seed'
        result = call('search', [method, start, tree, size * budget_multiple, prng, policy],
                      branch, stage, parent)
        if result['start_plan'] != start and start != 'default':
            raise AssertionError('search did not start from the branch parent plan')
        if result['restart'] != (policy == 'restart'):
            raise AssertionError('wrong warm/cold policy')
        return result['best_plan']

    for stage, batch in enumerate(BATCHES):
        added = output / f'batch-{stage}'
        added.mkdir()
        for row in selected:
            if row['batch'] != batch:
                continue
            source = root / row['path']
            for tree in (added, cumulative):
                dest = tree / row['path']
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, dest)
                if sha(dest) != row['sha256']:
                    raise ValueError('input changed during staging')
        cumulative_size = sum(r['size'] for r in selected if BATCHES.index(r['batch']) <= stage)
        added_size = sum(r['size'] for r in selected if r['batch'] == batch)
        stage_states = {}
        active = dict(branches)
        if stage:
            active.update({'R-seed': 'seed', 'R-coord': 'explicit'})
        for branch, mode in active.items():
            parent = states.get(branch) if not branch.startswith('R-') else None
            plan = initial_plan if stage == 0 else parent['plan'] if parent else 'default'
            out = artifacts / f'{stage}-{branch}.sadx'
            if stage and branch == 'B-new':
                plan = search(branch, stage, parent['plan'], added, 'local', parent, added_size)
            elif stage and branch in ('C', 'C-coord', 'R-seed', 'R-coord'):
                plan = search(branch, stage, plan, cumulative,
                              'restart' if branch.startswith('R-') else 'local',
                              parent if not branch.startswith('R-') else None, cumulative_size)
            is_append = stage and branch in ('A', 'B-new', 'D-nearest', 'S-segment', 'S-regroup')
            if is_append:
                args = [output / parent['artifact'], added, out]
                if branch == 'B-new':
                    args += ['--plan', plan]
                elif branch == 'S-regroup':
                    args += ['regroup']
                else:
                    args += ['segment']
                result = call('append', args, branch, stage, parent)
            else:
                result = call('pack', [mode, plan, cumulative, out, 'off'], branch, stage, parent)
            checked = call('verify', [out, cumulative], branch, stage, parent)
            if not checked.get('byte_exact'):
                raise AssertionError('byte-exact verification missing')
            keeps_prefix = bool(stage and is_append and branch != 'S-regroup')
            expected_gens = stage + 1 if branch in ('A', 'B-new', 'D-nearest', 'S-segment') else 1
            if checked['generations'] != expected_gens:
                raise AssertionError('history generations do not match branch semantics')
            if keeps_prefix and (not result.get('old_is_prefix') or
                                 not prefix_matches(output / parent['artifact'], out)):
                raise AssertionError('branch parent is not the exact output prefix')
            current = dict(stage=stage, branch=branch, plan=plan,
                           artifact=out.relative_to(output).as_posix(), bytes=out.stat().st_size,
                           sha256=sha(out), parent=parent['artifact'] if parent else None,
                           generations=checked['generations'], parent_prefix=keeps_prefix,
                           byte_exact=True)
            stage_states[branch] = current
            with (output / 'histories.jsonl').open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(current) + '\n')
        states = stage_states
    completion = dict(scope='functional acceptance only; not a performance result',
                      stages=len(BATCHES), commands=len(records), byte_exact=True,
                      independent_branches=list(branches), cold_controls=['R-seed', 'R-coord'])
    write_json(output / 'complete.json', completion)
    return completion


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--initial-plan', default='0x148')
    parser.add_argument('--budget-multiple', type=int, default=8)
    parser.add_argument('--prng-seed', type=int, default=3)
    args = parser.parse_args()
    if args.budget_multiple < 2 or not 0 <= args.prng_seed < 2**64:
        parser.error('need a uint64 PRNG seed and budget multiple >= 2')
    print(json.dumps(run(args.tool, args.input, args.manifest, args.output,
                         args.initial_plan, args.budget_multiple, args.prng_seed)))


if __name__ == '__main__':
    main()
