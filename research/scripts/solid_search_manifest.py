# SPDX-License-Identifier: GPL-3.0-or-later
"""Remove the artificial root only; preserve every content and cohort assignment."""
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
from append_history_r2 import sha, write_json


def key(rule, path, size):
    # Exact append_common.hpp::group_key semantics, including root-level files.
    first = path.find('/')
    if rule == 0:
        return '' if first < 0 else path[:first]
    if rule == 1:
        if first < 0:
            return ''
        second = path.find('/', first+1)
        return path[:first if second < 0 else second]
    if rule == 2:
        base = path.rsplit('/', 1)[-1]
        dot = base.rfind('.')
        ext = '' if dot <= 0 else base[dot+1:]
        return ''.join(chr(ord(c)+32) if 'A' <= c <= 'Z' else c for c in ext)
    return 's' if size <= 1024 else 'm' if size <= 16384 else 'l'


def main():
    prior = Path('research/datasets/append-linux-large-dev-r2/dev-linux-512.tsv')
    target = Path('research/datasets/solid-linux-dev-r1')
    target.mkdir(exist_ok=True)
    old = [line.split('\t') for line in prior.read_text().splitlines()]
    new = []
    for path, size, digest, group, batch in old:
        if not path.startswith('linux/') or not group.startswith('linux/'):
            raise ValueError('unexpected original prefix')
        rel = path.removeprefix('linux/')
        p = PurePosixPath(rel)
        if p.is_absolute() or '..' in p.parts or p.as_posix() != rel:
            raise ValueError('unsafe rewritten path')
        new.append([rel, size, digest, group.removeprefix('linux/'), batch])
    manifest = target/'dev.tsv'
    text = ''.join('\t'.join(r)+'\n' for r in new)
    if manifest.exists() and manifest.read_text(encoding='utf-8') != text:
        raise ValueError('refuse to replace a different manifest')
    manifest.write_text(text, encoding='utf-8')
    check = subprocess.run([sys.argv[1], '--manifest'], input=''.join(r[0]+'\n' for r in new),
                           capture_output=True, text=True, check=True)
    if not check.stdout.startswith('ok\t'):
        raise ValueError('production manifest check failed: '+check.stdout)
    def counts(data):
        return {name: len({key(i, r[0], int(r[1])) for r in data})
                for i, name in enumerate(['dir1','dir2','ext','size'])}
    write_json(target/'metadata.json', dict(scope='same dev contents and original cohorts; relative root changed only',
        prior_manifest_sha256=sha(prior), manifest_sha256=sha(manifest), files=len(new),
        bytes=sum(int(r[1]) for r in new), content_and_batch_preserved=True,
        batches={b: dict(files=sum(r[4]==b for r in new), bytes=sum(int(r[1]) for r in new if r[4]==b))
                 for b in ['initial','append1','append2','append3']},
        original_exact_key_counts=counts(old), new_exact_key_counts=counts(new),
        path_check=check.stdout.strip(), holdout_contents_read=False))
    print((target/'metadata.json').read_text())


if __name__ == '__main__':
    main()
