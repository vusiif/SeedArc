# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end acceptance of the history runner; requires the real v4 executable.

Creates only a fresh directory below --work. Artifacts may be removed by the caller
after copying evidence; existing paths are never reused or recursively removed here.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    runner = Path(__file__).with_name('append_history_r2.py')
    spec = importlib.util.spec_from_file_location('append_history_r2', runner)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    args.work.mkdir(parents=False, exist_ok=False)
    source = args.work / 'input'
    source.mkdir()
    manifest = args.work / 'manifest.tsv'
    lines = []
    for stage, batch in enumerate(module.BATCHES):
        for i in range(8):
            # Overlapping and new directory / extension keys in every stage.
            folder = ['src', 'docs', f'new-{stage}'][i % 3]
            rel = f'{folder}/文件-{stage}-{i}.txt'
            path = source / rel
            path.parent.mkdir(exist_ok=True)
            content = (f'phase {stage}, item {i}: alpha beta gamma key=value function()\n' * 60).encode()
            path.write_bytes(content)
            lines.append(f'{rel}\t{len(content)}\t{module.sha(path)}\t{folder}\t{batch}\n')
    manifest.write_text(''.join(lines), encoding='utf-8')
    results = []
    for repeat in range(2):
        out = args.work / f'run-{repeat}'
        cmd = [sys.executable, str(runner), '--tool', str(args.tool.resolve()),
               '--input', str(source), '--manifest', str(manifest), '--output', str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', timeout=300)
        if proc.returncode:
            raise AssertionError(f'runner failed: {proc.stdout}\n{proc.stderr}')
        results.append(json.loads(proc.stdout))
    first, second = args.work / 'run-0', args.work / 'run-1'
    histories = [json.loads(line) for line in (first / 'histories.jsonl').read_text().splitlines()]
    other = [json.loads(line) for line in (second / 'histories.jsonl').read_text().splitlines()]
    if histories != other:
        raise AssertionError('same input/history/seed did not reproduce all artifacts byte for byte')
    searches = [json.loads(line) for line in (first / 'raw.jsonl').read_text().splitlines()
                if json.loads(line)['command'] == 'search']
    by_id = {(r['stage'], r['branch']): r for r in histories}
    for row in histories:
        stage, branch = row['stage'], row['branch']
        expected_parent = None if stage == 0 or branch.startswith('R-') else f'artifacts/{stage-1}-{branch}.sadx'
        if row['parent'] != expected_parent:
            raise AssertionError('branches did not retain their own independent history')
    for row in searches:
        stage, branch, result = row['stage'], row['branch'], row['result']
        if branch.startswith('R-'):
            if row['parent_artifact'] is not None or not result['restart']:
                raise AssertionError('cold control accidentally warm-started')
        else:
            parent = by_id[stage - 1, branch]
            if row['parent_artifact'] != parent['artifact'] or result['start_plan'] != parent['plan'] or result['restart']:
                raise AssertionError('warm branch did not start at its own prior plan')
    # Manifest mismatch and cap failures must happen before any candidate output is created.
    damaged = args.work / 'bad.tsv'
    damaged.write_text(''.join(lines).replace(lines[0].split('\t')[2], '0' * 64, 1), encoding='utf-8')
    oversized = args.work / 'large.tsv'
    oversized.write_text('x\t8388609\t' + '0' * 64 + '\tx\tinitial\n')
    for bad in [damaged, oversized]:
        rejected = False
        try:
            module.load_manifest(source, bad)
        except ValueError:
            rejected = True
        if not rejected:
            raise AssertionError('bad manifest or unmet scale gate was silently accepted')
    summary = dict(scope='functional acceptance, not performance', passes=2,
                   artifacts_per_pass=len(histories), searches_per_pass=len(searches),
                   byte_identical_history=True, independent_parents=True,
                   chinese_paths=True, bad_manifest_rejected=True, scale_limit_rejected=True,
                   tool_sha256=module.sha(args.tool))
    module.write_json(args.work / 'acceptance.json', summary)
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
