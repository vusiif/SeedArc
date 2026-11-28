# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-040b: RAR on the same dev trees as DEV-040, measured locally (user decision 2026-11-27).

  python -I research/scripts/external_rar.py --rar RAR_EXE --sevenzip 7ZA --work EMPTY_DIR \
      --out RESULT_DIR TREE_NAME=DIR ...

RAR's compressor is proprietary; it runs only on the user's licensed local installation, so this
is not part of the India run. Rules, fixed before any run:
  * Sizes compare directly with DEV-040 (same input trees, checked against the upload manifest
    by the caller). Times and memory do not: different machine. A 7z -mx9 -mmt1 cell runs here as
    the local reference, so RAR's time is reported against 7z measured on the same machine.
  * Each tool keeps its default metadata (RAR and 7z store modification times; RAR adds its
    quick-open header copy by default). Size is the whole archive file.
  * Cells: rar a -ma5 -m5 -s -mt1; the same with -md1g (dictionary above every tree's size);
    -m5 -s with default threads (speed only). 7z -mx9 -mmt1 reference.
  * Every single-thread archive: full test (rar t / 7za t), then three files at 10 / 50 / 90 %
    of SeedArc's path order extracted and compared by SHA256, then the archive is deleted.
Helpers (run / peak memory / listing / picks) come from external_baseline.py.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

_spec = importlib.util.spec_from_file_location('external_baseline', Path(__file__).with_name('external_baseline.py'))
eb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(eb)


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--rar', required=True)
    a.add_argument('--sevenzip', required=True)
    a.add_argument('--work', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('trees', nargs='+')
    a = a.parse_args()
    rar, z7 = str(Path(a.rar).resolve(strict=True)), str(Path(a.sevenzip).resolve(strict=True))
    work, out = Path(a.work), Path(a.out)
    if work.exists() and any(work.iterdir()):
        raise SystemExit('work dir must be empty')
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    banner = subprocess.run([rar], capture_output=True).stdout.decode('utf-8', 'replace')
    first = next((l.split() for l in banner.splitlines() if l.strip().startswith('RAR')), [])
    versions = dict(rar=' '.join(first[:3]) or None,  # e.g. 'RAR 7.23 x64'; the rest is localised
                    rar_sha256=eb.sha(rar), sevenzip=next(
                        l for l in subprocess.run([z7, 'i'], capture_output=True, text=True).stdout.splitlines()
                        if '7-Zip' in l).strip(), python=sys.version.split()[0])
    (out / 'versions.json').write_text(json.dumps(versions, indent=1) + '\n', encoding='utf-8')
    results = open(out / 'raw.jsonl', 'a', encoding='utf-8')

    def emit(**row):
        results.write(json.dumps(row, ensure_ascii=False) + '\n')
        results.flush()
        print(json.dumps(row, ensure_ascii=False), flush=True)

    for spec in a.trees:
        name, path = spec.split('=', 1)
        root = Path(path).resolve(strict=True)
        rel = eb.listing(root)
        total = sum((root / r).stat().st_size for r in rel)
        picks = []
        for frac in (0.1, 0.5, 0.9):
            i = int(len(rel) * frac)
            while (root / rel[i]).stat().st_size == 0:
                i += 1
            picks.append(rel[i])
        emit(tree=name, cell='input', files=len(rel), input_bytes=total, picks=picks)
        d = work / name
        d.mkdir()
        lst = d / 'member.lst'

        def probe(cell, arc, kind):
            if kind == 'rar':
                m, _ = eb.run([rar, 't', '-idq', str(arc)])
            else:
                m, _ = eb.run([z7, 't', '-bd', '-bso0', '-bsp0', str(arc)])
            emit(tree=name, cell=cell, op='full decode', **m)
            for k, member in enumerate(picks):
                dest = d / f'pick{k}'
                if dest.exists():
                    shutil.rmtree(dest)
                dest.mkdir()
                if kind == 'rar':
                    # -scfl: the list file is UTF-8; `e` drops the stored path. RAR matches names
                    # with Windows separators.
                    lst.write_text(member.replace('/', '\\') + '\n', encoding='utf-8')
                    m, _ = eb.run([rar, 'e', '-idq', '-y', '-scfl', str(arc), f'@{lst}', str(dest) + '\\'])
                else:
                    lst.write_text(member + '\n', encoding='utf-8')
                    m, _ = eb.run([z7, 'e', '-bd', '-bso0', '-bsp0', '-spd', '-scsUTF-8', str(arc), f'-o{dest}',
                                   f'@{lst}'])
                got = dest / Path(member).name
                ok = got.is_file() and eb.sha(got) == eb.sha(root / member)
                emit(tree=name, cell=cell, op=f'extract {int((0.1, 0.5, 0.9)[k] * 100)}%', ok=ok, **m)
                if not ok:
                    raise SystemExit(f'extracted {member} differs ({cell})')
            arc.unlink()

        cells = [('rar -ma5 -m5 -s -mt1', ['-m5', '-s', '-mt1'], 1),
                 ('rar -ma5 -m5 -s -md1g -mt1', ['-m5', '-s', '-md1g', '-mt1'], 1),
                 ('rar -ma5 -m5 -s (default threads)', ['-m5', '-s'], 'all')]
        for k, (cell, flags, threads) in enumerate(cells):
            arc = d / f'tree{k}.rar'
            m, _ = eb.run([rar, 'a', '-ma5', *flags, '-r', '-idq', '-y', str(arc.resolve()), '*'], cwd=root)
            emit(tree=name, cell=cell, threads=threads, archive_bytes=arc.stat().st_size, **m)
            if threads == 1:
                probe(cell, arc, 'rar')
            else:
                arc.unlink()
        arc = d / 'tree.7z'
        m, _ = eb.run([z7, 'a', '-t7z', '-mx9', '-mmt1', '-bd', '-bso0', '-bsp0', str(arc.resolve()), '*'], cwd=root)
        emit(tree=name, cell='7z -mx9 -mmt1 (local reference)', threads=1, archive_bytes=arc.stat().st_size, **m)
        probe('7z -mx9 -mmt1 (local reference)', arc, '7z')
        shutil.rmtree(d)


if __name__ == '__main__':
    main()
