# SPDX-License-Identifier: GPL-3.0-or-later
"""Extend append-r1 public Linux dev metadata without opening holdout contents.

Keep the original content-connected group split and salt, remove the sampling
cap, deduplicate dev contents, and select nested file subsets up to 512 MiB.
The existing source archive index contains hashes; no source bytes are read here.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

from append_corpus import components, key_hash, batches


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--listing', type=Path, required=True)
    parser.add_argument('--prior-metadata', type=Path, required=True)
    parser.add_argument('--path-tool', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prior = json.loads(args.prior_metadata.read_text(encoding='utf-8'))
    excluded = {r[0] for r in prior['path_rejected']} | set(prior['casefold_dropped'])
    rows = []
    for line in args.listing.read_text(encoding='utf-8').splitlines():
        rel, size, digest = line.split('\t')
        if not rel.startswith('linux-6.12/'):
            raise ValueError('unexpected archive prefix')
        rel = 'linux/' + rel.removeprefix('linux-6.12/')
        path = PurePosixPath(rel)
        if path.is_absolute() or '..' in path.parts or path.as_posix() != rel:
            raise ValueError('unsafe indexed path')
        inner = path.parts[1:]
        group = 'linux/' + '/'.join(inner[:2] if len(inner) > 2 else inner[:1])
        if int(size) > 0 and rel not in excluded:
            rows.append(dict(rel=rel, size=int(size), sha=digest, group=group))
    comp = components(rows)
    dev = [r for r in rows if int(key_hash(comp[r['group']])[0], 16) % 2 == 0]
    seen, unique = set(), []
    for row in sorted(dev, key=lambda r: key_hash(r['rel'])):
        if row['sha'] not in seen:
            unique.append(row)
            seen.add(row['sha'])
    if sum(r['size'] for r in unique) < 512 * 1024**2:
        raise ValueError('not enough unique dev bytes; do not duplicate data or use holdout')
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(corpus_id='append-linux-large-dev-r2', scope='dev same-source append only',
                  source='Linux 6.12', source_url='https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz',
                  archive_sha256='b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb',
                  license='GPL-2.0; per-file SPDX applies', listing_sha256=sha(args.listing),
                  prior_metadata_sha256=sha(args.prior_metadata),
                  split='append-r1 content-connected components and original append-r1 salt; dev side only',
                  dedup='one path per content SHA256; no duplication or padding',
                  holdout_contents_read=False, unique_dev_bytes=sum(r['size'] for r in unique), scales=[])
    # A common ordered prefix makes scales nested without file truncation.
    for mib in [128, 256, 512]:
        selected, total = [], 0
        for row in unique:
            if total + row['size'] > mib * 1024**2:
                break
            selected.append(dict(row))
            total += row['size']
        if total < mib * 1024**2 * .95:
            raise ValueError('prefix undershoots requested scale by over five percent')
        paths = '\n'.join(r['rel'] for r in selected) + '\n'
        checked = subprocess.run([str(args.path_tool), '--manifest'], input=paths.encode(),
                                 capture_output=True, check=True)
        if not checked.stdout.decode().startswith('ok\t'):
            raise ValueError('production manifest check failed: ' + checked.stdout.decode())
        ordered = batches(selected, natural=False)
        target = args.output / f'dev-linux-{mib}.tsv'
        target.write_text(''.join(f"{r['rel']}\t{r['size']}\t{r['sha']}\t{r['group']}\t{r['batch']}\n"
                                  for r in ordered), encoding='utf-8')
        report['scales'].append(dict(target_mib=mib, files=len(ordered), bytes=total,
            groups=len({r['group'] for r in ordered}), manifest=target.name, sha256=sha(target),
            batches={b:dict(files=sum(r['batch']==b for r in ordered),
                           bytes=sum(r['size'] for r in ordered if r['batch']==b))
                     for b in ['initial', 'append1', 'append2', 'append3']},
            path_manifest_check=checked.stdout.decode().strip()))
    (args.output / 'metadata.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
