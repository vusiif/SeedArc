# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure research-v4 memory/disk capacity, not compression benefit.

Deterministic incompressible synthetic data exercise buffer copies. This is not a
representative Seed experiment or a substitute for the public dev corpus.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import subprocess


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def logical_disk(root):
    # Match the skill monitor: multiple hardlink names are counted separately.
    return sum(p.lstat().st_size for p in root.rglob('*') if p.is_file() and not p.is_symlink())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--sizes-mib', type=int, nargs='+', default=[128, 256, 512])
    args = parser.parse_args()
    if args.sizes_mib != sorted(set(args.sizes_mib)) or any(n <= 0 or n > 512 for n in args.sizes_mib):
        parser.error('use distinct increasing capacity steps up to 512 MiB; no input limit is raised')
    args.work.mkdir(exist_ok=False)
    args.results.mkdir(exist_ok=False)
    tool = args.tool.resolve(strict=True)
    root = args.work.resolve()
    results = args.results.resolve()
    raw = results / 'raw.jsonl'
    records, capacities = [], []
    affinity = {min(os.sched_getaffinity(0))} if hasattr(os, 'sched_getaffinity') else None
    (results / 'config.json').write_text(json.dumps(dict(scope='capacity only',
        tool_sha256=sha(tool), platform=platform.platform(), sizes_mib=args.sizes_mib,
        input='random.Random(0x5EEDA4C), one MiB generation buffer; two large files or four MiB files',
        memory_gate_bytes=2684354560, process_memory_limit='skill shared 3 GiB',
        disk_gate_bytes=2 * 1024**3, plan='0x0 (one group, no dictionary)',
        affinity=sorted(affinity) if affinity else None), indent=2))
    for size_mib in args.sizes_mib:
        if capacities:
            previous = [r for r in capacities if r['size_mib'] == max(x['size_mib'] for x in capacities)]
            worst = max(r['peak_memory_bytes'] for r in previous)
            prior_size = previous[0]['size_mib']
            predicted = int(worst * size_mib / prior_size * 1.2)
            if predicted > 2684354560:
                (results / 'capacity-gate.json').write_text(json.dumps(dict(
                    skipped_from_mib=size_mib, predicted_with_20pct_margin=predicted,
                    threshold=2684354560, reason='retain headroom below shared 3 GiB; do not raise limits'), indent=2))
                break
        for layout in ['two-large-files', 'four-mib-files']:
            case = root / f'{size_mib}-{layout}'
            case.mkdir()
            initial, added, cumulative = [case / name for name in ['initial', 'added', 'cumulative']]
            for directory in [initial, added, cumulative]:
                directory.mkdir()
            total = size_mib * 1024**2
            first_bytes = total * 7 // 10
            rng = random.Random(0x5EEDA4C)
            file_index = 0
            for name, amount in [('initial', first_bytes), ('added', total - first_bytes)]:
                target = initial if name == 'initial' else added
                while amount:
                    file_bytes = amount if layout == 'two-large-files' else min(amount, 4 * 1024**2)
                    path = target / f'{file_index:04d}.bin'
                    with path.open('xb') as stream:
                        remaining = file_bytes
                        while remaining:
                            n = min(remaining, 1024**2)
                            stream.write(rng.randbytes(n))
                            remaining -= n
                    os.link(path, cumulative / path.name)
                    amount -= file_bytes
                    file_index += 1
            inputs = {p.name:dict(bytes=p.stat().st_size, sha256=sha(p)) for p in cumulative.iterdir()}
            (results / f'{size_mib}-{layout}-inputs.json').write_text(json.dumps(inputs, indent=2))

            def call(label, cmd):
                disk_before = logical_disk(root)
                def fixed_cpu():
                    if affinity:
                        os.sched_setaffinity(0, affinity)
                proc = subprocess.run([str(tool), *map(str, cmd)], capture_output=True, text=True,
                                      timeout=600, preexec_fn=fixed_cpu if affinity else None)
                stem = f'{len(records):03d}-{size_mib}-{layout}-{label}'
                (results / (stem + '.stderr')).write_text(proc.stderr)
                (results / (stem + '.stdout')).write_text(proc.stdout)
                rec = dict(index=len(records), size_mib=size_mib, layout=layout, command=label,
                           argv=list(map(str, cmd)), returncode=proc.returncode,
                           disk_before=disk_before, disk_after=logical_disk(root))
                if proc.returncode == 0:
                    rec['result'] = json.loads(proc.stdout)
                records.append(rec)
                with raw.open('a') as stream:
                    stream.write(json.dumps(rec) + '\n')
                print(json.dumps(rec), flush=True)
                if proc.returncode:
                    raise RuntimeError(f'{stem} failed; preserve case for diagnosis')
                if rec['disk_after'] >= 2 * 1024**3:
                    raise RuntimeError('capacity disk gate exceeded')
                return rec['result']

            for mode, append_how in [('seed', 'segment'), ('solid', 'regroup')]:
                old, new = case / 'old.sadx', case / 'new.sadx'
                call(mode + '-pack', ['pack', mode, '0x0', initial, old, 'off'])
                call(mode + '-verify-initial', ['verify', old, initial])
                before_sha = sha(old)
                result = call(mode + '-append', ['append', old, added, new, append_how])
                checked = call(mode + '-verify-all', ['verify', new, cumulative])
                if sha(old) != before_sha or not checked.get('byte_exact'):
                    raise AssertionError('input archive changed or roundtrip failed')
                if mode == 'seed' and not result.get('old_is_prefix'):
                    raise AssertionError('append lost prefix property')
                (results / f'{size_mib}-{layout}-{mode}-artifacts.json').write_text(json.dumps(dict(
                    old_sha256=before_sha, new_sha256=sha(new), old_bytes=old.stat().st_size,
                    new_bytes=new.stat().st_size, byte_exact=True), indent=2))
                old.unlink()
                new.unlink()
            sample = [r for r in records if r['size_mib'] == size_mib and r['layout'] == layout]
            capacities.append(dict(size_mib=size_mib, layout=layout,
                peak_memory_bytes=max(r['result']['peak_memory_bytes'] for r in sample),
                logical_disk_peak=max(max(r['disk_before'], r['disk_after']) for r in sample)))
            # Only this newly created case, after archive verification; no shared corpus.
            if case.is_symlink() or case.resolve().parent != root:
                raise RuntimeError('unexpected case root')
            shutil.rmtree(case)
            (results / 'capacity-summary.json').write_text(json.dumps(capacities, indent=2))
    (results / 'complete.json').write_text(json.dumps(dict(status='capacity probe complete',
        commands=len(records), cases=len(capacities), large_scale_claim=False), indent=2))


if __name__ == '__main__':
    main()
