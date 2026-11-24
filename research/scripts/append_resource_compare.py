# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare complete capacity cases; fail if inputs or archive bytes differ.

Arguments are baseline and after probe result directories. Partial cases are
reported separately and never counted as a successful scale.
"""
import argparse
import json
from pathlib import Path


def load(root):
    records = [json.loads(line) for line in (root / 'raw.jsonl').read_text().splitlines()]
    if any(r['returncode'] != 0 for r in records):
        raise ValueError('nonzero command in capacity probe')
    summary = json.loads((root / 'capacity-summary.json').read_text())
    cases = {}
    for row in summary:
        key = (row['size_mib'], row['layout'])
        sample = [r for r in records if (r['size_mib'], r['layout']) == key]
        wanted = {mode + '-' + operation for mode in ['seed', 'solid']
                  for operation in ['pack', 'verify-initial', 'append', 'verify-all']}
        if len(sample) != 8 or {r['command'] for r in sample} != wanted:
            raise ValueError('incomplete command set for completed case')
        if any(not r['result'].get('byte_exact') for r in sample if 'verify' in r['command']):
            raise ValueError('missing successful byte comparison')
        if row['peak_memory_bytes'] != max(r['result']['peak_memory_bytes'] for r in sample):
            raise ValueError('peak memory summary mismatch')
        cases[key] = row
    return records, cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    before_records, before = load(args.baseline)
    after_records, after = load(args.after)
    rows = []
    for key in sorted(before.keys() & after.keys()):
        stem = f'{key[0]}-{key[1]}'
        for suffix in ['inputs', 'seed-artifacts', 'solid-artifacts']:
            name = stem + '-' + suffix + '.json'
            if json.loads((args.baseline / name).read_text()) != json.loads((args.after / name).read_text()):
                raise ValueError('input or archive mismatch: ' + name)
        old, new = before[key]['peak_memory_bytes'], after[key]['peak_memory_bytes']
        rows.append(dict(size_mib=key[0], layout=key[1], before_peak_bytes=old,
                         after_peak_bytes=new, reduction_percent=100 * (old - new) / old,
                         inputs_and_archives_identical=True))
    print(json.dumps(dict(scope='single-run resource observation, not Seed benefit or timing claim',
        comparison=rows, after_completed_cases=list(after.values()),
        before_commands=len(before_records), after_commands=len(after_records),
        after_partial_commands=sum((r['size_mib'], r['layout']) not in after for r in after_records)), indent=2))


if __name__ == '__main__':
    main()
