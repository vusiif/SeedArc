# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-036: dup-local-dev-r1, a local-only dev dataset that keeps natural duplicates.

The DEV-023 trees keep one path per content (append-linux-large-dev-r2), so they cannot show how
common whole-file duplicates are. This dataset takes three local, open-source / package trees
that no holdout or test set was built from (user choice 2026-11-27):

  cargo   ~/.cargo/registry/src/<index>/<crate>-<version>/...   group: crate name (all versions)
  rustup  ~/.rustup/toolchains/<toolchain>/...                  group: first 3 path components
                                                                 inside the toolchain, shared by
                                                                 all toolchains
  npm     %APPDATA%/npm/node_modules/<package>/...              group: package (@scope/name)

Rules (fixed before any compression run):
  * regular files only (no symlinks / junctions), 1 B to 32 MiB, not an already-compressed
    format (COMPRESSED suffixes);
  * content whose SHA256 appears in any holdout or test manifest (append-r1 holdout, g5-holdout-r1,
    b2r5-corpus-r1/r2 test) is dropped;
  * per pool: groups ordered by sha256("dup-local-dev-r1:<group>"), whole groups taken while the
    pool stays <= 256 MiB; a group that does not fit is skipped;
  * every path passes seedarc_path_check (ADR-0005), a casefold() collision filter and the
    production manifest check (--manifest) for the whole dataset.
Population statistics (before selection) are recorded too: duplicate files / bytes per pool.

  python -I research/scripts/dup_local_corpus.py --path-check <seedarc_path_check>
  python -I research/scripts/dup_local_corpus.py --check
Content is copied to the ignored research/local/dup-local-dev-r1/files/ and never uploaded;
only relative paths, sizes, SHA256 and groups are committed.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CORPUS = 'dup-local-dev-r1'
LOCAL = ROOT / 'research/local' / CORPUS / 'files'
META = ROOT / 'research/datasets' / CORPUS
MiB = 1024 * 1024
POOL_CAP = 256 * MiB
FILE_MAX = 32 * MiB
HOME = Path.home()
SOURCES = {
    'cargo': HOME / '.cargo/registry/src',
    'rustup': HOME / '.rustup/toolchains',
    'npm': Path(os.environ.get('APPDATA', '')) / 'npm/node_modules',
}
COMPRESSED = {'.gz', '.tgz', '.xz', '.txz', '.zst', '.zip', '.crate', '.7z', '.bz2', '.tbz2', '.br', '.lz4',
              '.lzma', '.jar', '.whl', '.png', '.jpg', '.jpeg', '.gif', '.webp', '.woff', '.woff2'}
HOLDOUT_GLOBS = ['append-r1/holdout-*.tsv', 'g5-holdout-r1/*.tsv', 'b2r5-corpus-r1/test-*.tsv',
                 'b2r5-corpus-r2/test-*.tsv']
HEX64 = re.compile(r'\b[0-9a-f]{64}\b')
VERSION = re.compile(r'^(.+?)-\d+\.\d+\.\d+.*$')


def sha_file(path):
    with open(path, 'rb') as s:
        return hashlib.file_digest(s, 'sha256').hexdigest()


def key_hash(text):
    return hashlib.sha256(f'{CORPUS}:{text}'.encode()).hexdigest()


def walk(root):
    """Regular files under root, never following symlinks or junctions."""
    stack = [root]
    while stack:
        d = stack.pop()
        with os.scandir(d) as it:
            for e in it:
                p = Path(e.path)
                if e.is_symlink() or p.is_junction():
                    continue
                if e.is_dir(follow_symlinks=False):
                    stack.append(p)
                elif e.is_file(follow_symlinks=False):
                    yield p


def candidates(report):
    pools = defaultdict(list)
    skipped = defaultdict(int)
    for pool, root in SOURCES.items():
        if not root.is_dir():
            raise SystemExit(f'missing source for {pool}')
        bases = [next(p for p in root.iterdir() if p.is_dir())] if pool == 'cargo' else [root]
        if pool == 'cargo' and len([p for p in root.iterdir() if p.is_dir()]) != 1:
            raise SystemExit('expected exactly one cargo registry index')
        for base in bases:
            for p in walk(base):
                rel_in = p.relative_to(base).as_posix()
                parts = rel_in.split('/')
                n = p.stat().st_size
                if n == 0 or n > FILE_MAX:
                    skipped[f'{pool}: size'] += 1
                    continue
                if p.suffix.lower() in COMPRESSED:
                    skipped[f'{pool}: compressed format'] += 1
                    continue
                if pool == 'cargo':
                    m = VERSION.match(parts[0])
                    group = m.group(1) if m else parts[0]
                elif pool == 'rustup':
                    inner = parts[1:]
                    group = '/'.join(inner[:3]) if len(inner) > 3 else '/'.join(inner[:-1]) or '.'
                else:
                    group = '/'.join(parts[:2]) if parts[0].startswith('@') else parts[0]
                pools[pool].append(dict(rel=f'{pool}/{rel_in}', group=f'{pool}/{group}', size=n, src=p))
    report['skipped'] = dict(sorted(skipped.items()))
    return pools


def holdout_hashes():
    found = set()
    files = []
    for pattern in HOLDOUT_GLOBS:
        for f in sorted((ROOT / 'research/datasets').glob(pattern)):
            files.append(f.relative_to(ROOT).as_posix())
            found |= set(HEX64.findall(f.read_text(encoding='utf-8')))
    if not files:
        raise SystemExit('no holdout manifests found')
    return found, files


def dup_stats(rows):
    seen, files, nbytes = set(), 0, 0
    for r in sorted(rows, key=lambda r: r['rel']):
        if r['sha'] in seen:
            files += 1
            nbytes += r['size']
        seen.add(r['sha'])
    return dict(files=len(rows), bytes=sum(r['size'] for r in rows), duplicate_files=files,
                duplicate_bytes=nbytes, distinct_contents=len(seen))


def path_verdicts(tool, paths, manifest=False):
    data = ''.join(p + '\n' for p in paths).encode('utf-8')
    out = subprocess.run([tool] + (['--manifest'] if manifest else []), input=data, capture_output=True,
                         check=True).stdout.decode('utf-8')
    if manifest:
        return out.strip()
    return {line.split('\t')[1]: line.split('\t')[0] for line in out.splitlines() if line}


def casefold_drop(rows):
    keys = defaultdict(set)
    for r in rows:
        parts = r['rel'].split('/')
        for i in range(1, len(parts) + 1):
            keys['/'.join(parts[:i]).casefold()].add('/'.join(parts[:i]))
    bad = {k for k, v in keys.items() if len(v) > 1}
    keep, dropped = [], []
    for r in rows:
        parts = r['rel'].split('/')
        (dropped if any('/'.join(parts[:i]).casefold() in bad for i in range(1, len(parts) + 1)) else keep).append(r)
    return keep, [r['rel'] for r in dropped]


def build(tool):
    if LOCAL.exists() or META.exists():
        raise SystemExit('refuse to overwrite an existing dataset build')
    report = dict(dataset=CORPUS, task='DEV-036', scope='local-only dev; not a holdout', rules=__doc__,
                  pool_cap=POOL_CAP, file_max=FILE_MAX, compressed_suffixes=sorted(COMPRESSED),
                  sources={k: '~/' + v.relative_to(HOME).as_posix() if v.is_relative_to(HOME) else str(v)
                           for k, v in SOURCES.items()})
    pools = candidates(report)
    held, held_files = holdout_hashes()
    report['holdout_manifests'] = held_files
    report['population'], report['pools'], report['holdout_dropped'] = {}, {}, {}
    rows_all = []
    for pool, rows in pools.items():
        for r in rows:
            r['sha'] = sha_file(r['src'])
        report['population'][pool] = dup_stats(rows)
        clean = [r for r in rows if r['sha'] not in held]
        report['holdout_dropped'][pool] = len(rows) - len(clean)
        verdicts = path_verdicts(tool, [r['rel'] for r in clean])
        clean = [r for r in clean if verdicts.get(r['rel']) == 'ok']
        report.setdefault('path_rejected', {})[pool] = len([v for v in verdicts.values() if v != 'ok'])
        clean, dropped = casefold_drop(clean)
        report.setdefault('casefold_dropped', {})[pool] = dropped
        groups = defaultdict(list)
        for r in clean:
            groups[r['group']].append(r)
        chosen, total, skipped = [], 0, 0
        for g in sorted(groups, key=key_hash):
            size = sum(r['size'] for r in groups[g])
            if total + size > POOL_CAP:
                skipped += 1
                continue
            chosen += groups[g]
            total += size
        chosen.sort(key=lambda r: r['rel'])
        report['pools'][pool] = dict(groups_available=len(groups), groups_taken=len({r['group'] for r in chosen}),
                                     groups_skipped=skipped, **dup_stats(chosen))
        rows_all += chosen
    check = path_verdicts(tool, [r['rel'] for r in rows_all], manifest=True)
    report['manifest_check'] = check
    if not check.startswith('ok'):
        raise SystemExit('manifest check failed: ' + check)
    report['all'] = dup_stats(rows_all)
    for r in rows_all:
        dst = LOCAL / r['rel']
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(r['src'], dst)
        if dst.stat().st_size != r['size'] or sha_file(dst) != r['sha']:
            raise SystemExit('copy changed ' + r['rel'])
    META.mkdir(parents=True)
    for pool in pools:
        text = ''.join(f"{r['rel']}\t{r['size']}\t{r['sha']}\t{r['group']}\n" for r in rows_all
                       if r['rel'].startswith(pool + '/'))
        (META / f'{pool}.tsv').write_text(text, encoding='utf-8', newline='\n')
        report['pools'][pool]['manifest_sha256'] = hashlib.sha256(text.encode()).hexdigest()
    (META / 'metadata.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n',
                                        encoding='utf-8', newline='\n')
    print(json.dumps({k: report[k] for k in ['population', 'pools', 'holdout_dropped', 'all']}, indent=1))


def check():
    held, _ = holdout_hashes()
    n = 0
    for f in sorted(META.glob('*.tsv')):
        for line in f.read_text(encoding='utf-8').splitlines():
            rel, size, digest, _group = line.split('\t')
            if digest in held:
                raise SystemExit('holdout content in ' + rel)
            p = LOCAL / rel
            if not p.is_file() or p.stat().st_size != int(size) or sha_file(p) != digest:
                raise SystemExit('local copy differs: ' + rel)
            n += 1
    extra = sum(1 for p in LOCAL.rglob('*') if p.is_file()) - n
    if extra:
        raise SystemExit(f'{extra} files not in the manifests')
    print(f'ok: {n} files, no holdout content')


if __name__ == '__main__':
    a = argparse.ArgumentParser()
    a.add_argument('--path-check')
    a.add_argument('--check', action='store_true')
    a = a.parse_args()
    if a.check:
        check()
    elif a.path_check:
        build(str(Path(a.path_check).resolve(strict=True)))
    else:
        sys.exit('usage: --path-check TOOL | --check')
