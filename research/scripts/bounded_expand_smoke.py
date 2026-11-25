# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-023 runner smoke: fixed small tree (empty file, Chinese paths, small files, a file exactly
at the smallest cap) through both batches, plus an over-cap refusal that must leave no output.
Bytes are written in binary (UTF-8 / LF), so Windows and Linux see identical inputs."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from bounded_expand_dev import run_unit, sha, write_json

CAPS = [1024**3, 4096, 8192, 16384]


def build(root):
    root.mkdir(exist_ok=False)
    files = {'empty': b'', 'exact': b'X' * 4096}
    for i in range(24):
        files[f'源-{i % 5}/文件-{i}.txt'] = (f'data-{i}: alpha beta 中文\n' * (i * 9 + 30)).encode('utf-8')[:3000]
    lines = []
    for rel in sorted(files):
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(files[rel])
        k = hashlib.sha256(('smoke:' + rel).encode()).digest()[0]
        lines.append(f"{rel}\t{len(files[rel])}\t{hashlib.sha256(files[rel]).hexdigest()}\tsmoke\t{int(k < 40)}\t{int(k < 110)}\n")
    return ''.join(lines)


def main():
    tool, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(exist_ok=False)
    data = out / 'data'
    manifest = out / 'smoke.tsv'
    manifest.write_text(build(data), encoding='utf-8', newline='\n')
    report = dict(tool_sha256=sha(tool), manifest_sha256=sha(manifest), units=[])
    for batch in ['added10', 'added40']:
        report['units'].append(run_unit(tool, data, manifest, 'smoke', batch, out / f'work-{batch}',
                                        out / f'results-{batch}', out, caps=CAPS, disk_gate=1024**3))
    over = out / 'over'
    (over / 'd').mkdir(parents=True)
    (over / 'd' / 'big').write_bytes(b'Y' * 4097)
    target = over / 'out.sadx'
    proc = subprocess.run([str(tool), 'bounded-pack', 'seed', '4096', str(over / 'd'), str(target)],
                          capture_output=True, text=True, encoding='utf-8')
    report['over_cap'] = dict(returncode=proc.returncode, stderr=proc.stderr.strip()[:400],
                              output_exists=target.exists(), stdout=proc.stdout.strip()[:400])
    if proc.returncode == 0 or target.exists() or proc.stdout.strip():
        raise AssertionError('over-cap input was not refused cleanly')
    hashes = {}
    for batch in ['added10', 'added40']:
        for a in json.loads((out / f'results-{batch}' / 'archives.json').read_text(encoding='utf-8')):
            hashes[f"{batch}/{a['label']}"] = a['sha256']
    report['archive_hashes'] = hashes
    write_json(out / 'smoke-report.json', report)
    print(json.dumps(dict(units=report['units'], over_cap=report['over_cap']), ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main()
