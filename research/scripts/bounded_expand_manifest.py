# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-023: build the source / text dev trees and the 10% / 40% added batches.

Every rule here depends only on paths, sizes and file bytes, never on compression
results. The rules are fixed before any DEV-023 compression command runs:

* source: Linux 6.12 files from the existing 512 MiB dev manifest whose extension is a
  programming-language source extension (SOURCE_EXTENSIONS). Order them by
  sha256("dev023-select:" + path) and take the longest prefix whose total is at most
  256 MiB (stop at the first file that does not fit; no truncation, no padding).
* text: the existing enwik8 dev slices and the Silesia dev files that pass is_text().
  All qualifying files are used; the class may stay below 128 MiB.
* per-file limit 32 MiB, applied by size before anything is measured; excluded rows are
  listed in metadata.json with the reason.
* added batch for ratio r: order the final tree by sha256("dev023-batch:" + path) and
  take the prefix k (1 <= k < n) whose cumulative bytes are closest to r * total
  (ties: smaller k). The parent is the rest. Whole files only.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

SOURCE_EXTENSIONS = ('.c', '.h', '.S', '.rs', '.asm', '.py', '.sh', '.pl')
SOURCE_LIMIT = 256 * 1024**2
FILE_LIMIT = 32 * 1024**2
RATIOS = (('added10', 0.10), ('added40', 0.40))
SILESIA_DEV = ('nci', 'reymont', 'samba', 'sao', 'xml')
ALLOWED_CONTROL = {9, 10, 12, 13}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def key(prefix, rel):
    return hashlib.sha256((prefix + rel).encode('utf-8')).hexdigest()


def is_text(data):
    """UTF-8 with at most 4 replacement characters (slice boundaries), no NUL, and
    other C0 controls / DEL at most 0.1% of bytes."""
    if b'\0' in data:
        return False, 'contains NUL'
    replaced = data.decode('utf-8', errors='replace').count('�')
    if replaced > 4:
        return False, f'{replaced} invalid UTF-8 sequences'
    controls = sum(1 for b in data if (b < 32 and b not in ALLOWED_CONTROL) or b == 127)
    if controls * 1000 > len(data):
        return False, f'{controls} control bytes'
    return True, 'text'


def checked_rel(rel):
    p = PurePosixPath(rel)
    if p.is_absolute() or '..' in p.parts or p.as_posix() != rel or '\\' in rel or ':' in rel:
        raise ValueError('unsafe path ' + rel)
    return rel


def assign(rows):
    total = sum(r['size'] for r in rows)
    order = sorted(rows, key=lambda r: key('dev023-batch:', r['rel']))
    report = {}
    for name, ratio in RATIOS:
        target, cumulative, best = ratio * total, 0, None
        for k, row in enumerate(order[:-1], 1):
            cumulative += row['size']
            if best is None or abs(cumulative - target) < best[0]:
                best = (abs(cumulative - target), k, cumulative)
        chosen = {r['rel'] for r in order[:best[1]]}
        for r in rows:
            r[name] = int(r['rel'] in chosen)
        report[name] = dict(target_ratio=ratio, files=best[1], bytes=best[2], actual_ratio=best[2] / total)
    return report


def path_check(tool, rows):
    paths = ''.join(r['rel'] + '\n' for r in rows)
    out = subprocess.run([str(tool), '--manifest'], input=paths.encode('utf-8'),
                         capture_output=True, check=True).stdout.decode()
    if not out.startswith('ok\t'):
        raise ValueError('production manifest check failed: ' + out)
    return out.strip()


def write(rows, path):
    rows = sorted(rows, key=lambda r: r['rel'])
    path.write_text(''.join(f"{r['rel']}\t{r['size']}\t{r['sha256']}\t{r['origin']}\t{r['added10']}\t{r['added40']}\n"
                            for r in rows), encoding='utf-8', newline='\n')
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--linux-manifest', type=Path, required=True)
    ap.add_argument('--wiki-dir', type=Path, required=True, help='directory holding enwik8/part-*.xml')
    ap.add_argument('--silesia-dir', type=Path, required=True, help='directory holding Silesia dev files')
    ap.add_argument('--mixed-manifest', type=Path, required=True, help='existing v0.tsv for hash cross-check')
    ap.add_argument('--path-tool', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    a.output.mkdir(exist_ok=False)
    excluded = []

    # Source class.
    linux = []
    for line in a.linux_manifest.read_text(encoding='utf-8').splitlines():
        rel, size, digest, _group, _batch = line.split('\t')
        rel, size = checked_rel(rel), int(size)
        if PurePosixPath(rel).suffix not in SOURCE_EXTENSIONS:
            continue
        if size > FILE_LIMIT:
            excluded.append(dict(cls='source', rel=rel, size=size, reason='over 32 MiB'))
            continue
        linux.append(dict(rel=rel, size=size, sha256=digest, origin='linux-6.12'))
    linux.sort(key=lambda r: key('dev023-select:', r['rel']))
    source, total = [], 0
    for r in linux:
        if total + r['size'] > SOURCE_LIMIT:
            break
        source.append(r)
        total += r['size']
    candidates = len(linux)

    # Text class.
    known = {}
    for line in a.mixed_manifest.read_text(encoding='utf-8').splitlines():
        rel, size, digest, *_ = line.split('\t')
        known[rel] = (int(size), digest)
    text, decisions = [], []
    files = [('enwik8/' + p.name, p, 'enwik8-dev-slice') for p in sorted((a.wiki_dir / 'enwik8').glob('part-*.xml'))]
    files += [('silesia/' + n, a.silesia_dir / n, 'silesia-dev') for n in SILESIA_DEV]
    for rel, path, origin in files:
        checked_rel(rel)
        if path.is_symlink() or not path.is_file():
            raise ValueError('missing or linked input ' + str(path))
        size = path.stat().st_size
        if size > FILE_LIMIT:
            excluded.append(dict(cls='text', rel=rel, size=size, reason='over 32 MiB'))
            continue
        ok, why = is_text(path.read_bytes())
        decisions.append(dict(rel=rel, size=size, text=ok, reason=why))
        if not ok:
            excluded.append(dict(cls='text', rel=rel, size=size, reason='not text: ' + why))
            continue
        digest = sha(path)
        if rel in known and known[rel] != (size, digest):
            raise ValueError('content differs from the existing dev manifest: ' + rel)
        text.append(dict(rel=rel, size=size, sha256=digest, origin=origin))

    report = dict(dataset='bounded-expand-dev-r1', task='DEV-023', scope='existing public dev only; not a holdout',
                  rules=__doc__, source_extensions=SOURCE_EXTENSIONS, file_limit=FILE_LIMIT,
                  source_limit=SOURCE_LIMIT, inputs={k: dict(path=str(v).replace('\\', '/'), sha256=sha(v)) for k, v in
                      [('linux_manifest', a.linux_manifest), ('mixed_manifest', a.mixed_manifest)]},
                  text_decisions=decisions, excluded=excluded, classes={})
    for name, rows, extra in [('source', source, dict(candidates=candidates)), ('text', text, {})]:
        if len({r['sha256'] for r in rows}) != len(rows):
            raise ValueError('duplicate content in ' + name)
        batches = assign(rows)
        check = path_check(a.path_tool, rows)
        target = a.output / f'{name}.tsv'
        rows = write(rows, target)
        report['classes'][name] = dict(files=len(rows), bytes=sum(r['size'] for r in rows),
            max_file=max(r['size'] for r in rows), manifest=target.name, sha256=sha(target),
            batches=batches, path_manifest_check=check, **extra)
    (a.output / 'metadata.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n',
                                            encoding='utf-8', newline='\n')
    print(json.dumps(report['classes'], indent=2))


if __name__ == '__main__':
    main()
