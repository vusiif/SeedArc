# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-045: how much archive size buys how much speed (frame size x dictionary x workers).

  python -I research/scripts/speed_matrix.py --tool SEEDARC_APPEND4_STUDY --work EMPTY_DIR \
      --out RESULT_DIR --sevenzip 7Z --xz XZ [--pin-cpu N] TREE_NAME=DIR ...

Rules (fixed before any run):
  * Same machine and trees as DEV-040 (India, exclusive task, nothing else measured at the same
    time); one run per cell. Measurement helpers (process wall clock, wait4 max RSS, tar stage,
    stream extraction) are external_baseline.py's, unchanged.
  * SeedArc cells: LZMA2-9e with whole-file dedup at frame 32 / 64 / 128 / 256 MiB and whole, the
    dictionary never larger than the frame (preset 64 MiB up to 64 MiB frames, 128 MiB from
    128 MiB, 128 and 256 MiB at 256 MiB and whole). Framed cells use `--jobs auto` with the
    ADR-0009 default budget (half the physical memory); whole cells have one frame and run with
    `--jobs 1` pinned to one CPU. When the default budget was what limited the workers, the same
    recipe runs again with a 75% budget, but only if that budget admits more workers by the
    tool's own formula (computed from the first run's report). Archives from both runs must be
    byte-identical (worker count never changes the bytes).
  * Decode: `verify ARCHIVE TREE`, pinned. Its reported `decode_wall_ms` (parse + decode + CRC,
    with a byte comparison against the tree already in memory) excludes reading the source
    tree; that is the decode time reported. The process wall time is recorded too.
  * Single-file extraction: the three DEV-040 picks (10% / 50% / 90% of the path order), once per
    recipe, pinned; every extracted file is compared with the source by SHA256.
  * External cells (same dictionary sizes where they exist): 7-Zip -mx9 -md=256m with one thread
    (pinned) and with its default threads; tar | xz LZMA2 preset 9e with a 256 MiB dictionary,
    one thread (pinned); tar | xz -9e -T0 --block-size=64MiB with --memlimit-compress=75%
    (xz's own block-parallel mode, the analogue of 64 MiB frames). Single-thread external cells
    get the same decode / extraction probes; multi-thread external cells record size and time.
  * Size: whole archive file bytes. The smallest cell is the reference for "size cost"; speed
    is compared by pack wall clock.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import external_baseline as eb  # noqa: E402  (same directory; run with -I, so put it on the path)

SEEDARC = [  # recipe, framed (jobs auto) or whole (jobs 1)
    ('solid/lzma2-9e/g1-r0/frame-32MiB/dedup', True),
    ('solid/lzma2-9e/g1-r0/frame-64MiB/dedup', True),
    ('solid/lzma2-9e-dict128MiB/g1-r0/frame-128MiB/dedup', True),
    ('solid/lzma2-9e-dict128MiB/g1-r0/frame-256MiB/dedup', True),
    ('solid/lzma2-9e-dict256MiB/g1-r0/frame-256MiB/dedup', True),
    ('solid/lzma2-9e-dict128MiB/g1-r0/frame-whole/dedup', False),
    ('solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup', False),
]


def mem_total():
    if eb.WINDOWS:  # local smoke runs only; the measurement is on Linux
        import ctypes

        class Status(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [
                (n, ctypes.c_uint64) for n in ('phys', 'avail', 'pf', 'apf', 'virt', 'avirt', 'ext')]
        s = Status()
        s.len = ctypes.sizeof(s)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s))
        return s.phys
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemTotal:'):
            return int(line.split()[1]) * 1024
    raise RuntimeError('no MemTotal')


def main():
    a = argparse.ArgumentParser()
    for opt in ('--tool', '--work', '--out', '--sevenzip', '--xz'):
        a.add_argument(opt, required=True)
    a.add_argument('--pin-cpu', type=int)
    a.add_argument('trees', nargs='+')
    a = a.parse_args()
    tool = str(Path(a.tool).resolve(strict=True))
    z7, xz = a.sevenzip, a.xz
    work, out = Path(a.work), Path(a.out)
    if work.exists() and any(work.iterdir()):
        raise SystemExit('work dir must be empty')
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    if a.pin_cpu is not None:
        eb.PIN.extend(['taskset', '-c', str(a.pin_cpu)])
    physical = mem_total()
    budget75 = physical * 3 // 4
    versions = dict(seedarc_tool_sha256=eb.sha(tool), mem_total_bytes=physical, budget75_bytes=budget75,
                    sevenzip=next(l for l in subprocess.run([z7, 'i'], capture_output=True, text=True).stdout.splitlines()
                                  if '7-Zip' in l).strip(),
                    xz=subprocess.run([xz, '--version'], capture_output=True, text=True).stdout.strip().replace('\n', '; '),
                    pin=eb.PIN, python=sys.version.split()[0], cpu_count=os.cpu_count())
    (out / 'versions.json').write_text(json.dumps(versions, indent=1) + '\n', encoding='utf-8')
    rows = open(out / 'raw.jsonl', 'a', encoding='utf-8')

    def emit(**row):
        rows.write(json.dumps(row, ensure_ascii=False) + '\n')
        rows.flush()
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
        d = work / name
        d.mkdir()
        emit(tree=name, cell='input', files=len(rel), input_bytes=total, picks=picks)

        def probe(cell, arc, kind):
            if kind == 'seedarc':
                with open(d / 'verify.json', 'wb') as f:
                    m, _ = eb.run([tool, 'verify', str(arc), str(root)], stdout=f, pin=True)
                info = json.loads((d / 'verify.json').read_text(encoding='utf-8'))
                m.update(decode_wall_s=round(info['decode_wall_ms'] / 1000, 3),
                         input_read_wall_s=round(info['input_read_wall_ms'] / 1000, 3))
            elif kind == '7z':
                m, _ = eb.run([z7, 't', '-bd', '-bso0', '-bsp0', str(arc)], pin=True)
            else:
                m, _ = eb.run([xz, '-dc', '-T1', str(arc)], pin=True)
            emit(tree=name, cell=cell, op='full decode', **m)
            for k, member in enumerate(picks):
                dest = d / f'pick{k}'
                if dest.exists():
                    shutil.rmtree(dest) if dest.is_dir() else dest.unlink()
                if kind == 'seedarc':
                    dest.mkdir()
                    m, _ = eb.run([tool, 'extract', str(arc), member, str(dest), 'out'], pin=True)
                    got = dest / 'out'
                elif kind == '7z':
                    dest.mkdir()
                    lst = d / 'member.lst'
                    lst.write_text(member + '\n', encoding='utf-8')
                    m, _ = eb.run([z7, 'e', '-bd', '-bso0', '-bsp0', '-spd', '-scsUTF-8', str(arc), f'-o{dest}',
                                   f'@{lst}'], pin=True)
                    got = dest / Path(member).name
                else:
                    m = eb.stream_extract([xz, '-dc', '-T1'], arc, member, dest)
                    got = dest
                ok = eb.sha(got) == eb.sha(root / member)
                emit(tree=name, cell=cell, op=f'extract {int((0.1, 0.5, 0.9)[k] * 100)}%', ok=ok, **m)
                if not ok:
                    raise SystemExit(f'extracted {member} differs ({cell})')
                shutil.rmtree(dest) if dest.is_dir() else dest.unlink()

        def pack(recipe, jobs, arc, budget=None, pin=False):
            pre = ['--jobs', jobs] + (['--memory-budget', str(budget)] if budget else [])
            with open(d / 'pack.json', 'wb') as f:
                m, err = eb.run([tool, *pre, 'recipe-pack', 'seed', recipe, str(root), str(arc)], stdout=f, pin=pin)
            info = json.loads((d / 'pack.json').read_text(encoding='utf-8'))
            auto = None
            if jobs == 'auto':
                auto = json.loads(err.decode().strip().splitlines()[-1])['jobs_auto']
            row = dict(tree=name, cell='seedarc ' + recipe, recipe=recipe, jobs=jobs, budget_bytes=budget,
                       archive_bytes=arc.stat().st_size, archive_sha256=eb.sha(arc),
                       pack_wall_s=round(info['pack_wall_ms'] / 1000, 3), jobs_auto=auto, **m)
            emit(**row)
            return row

        for recipe, framed in SEEDARC:
            arc = d / 'seedarc.sadx'
            first = pack(recipe, 'auto' if framed else '1', arc, pin=not framed)
            probe('seedarc ' + recipe, arc, 'seedarc')
            auto = first['jobs_auto']
            if auto and auto['limited_by'] in ('memory', 'memory_over_budget'):
                # Same formula as the tool, with the resident / reserve the first run reported.
                avail = max(0, budget75 - auto['resident_bytes'] - auto['reserve_bytes'])
                cap = max(1, min(16, os.cpu_count() or 1, auto['frames']))
                if min(cap, avail // auto['per_worker_bytes']) > auto['chosen']:
                    arc.unlink()
                    again = pack(recipe, 'auto', arc, budget=budget75)
                    if again['archive_sha256'] != first['archive_sha256']:
                        raise SystemExit(f'{recipe}: bytes depend on the worker count')
                else:
                    emit(tree=name, cell='seedarc ' + recipe, op='75% budget skipped',
                         reason='admits no more workers', budget_bytes=budget75)
            arc.unlink()

        tar_path = d / 'tree.tar'
        tar_s = eb.write_tar(root, rel, tar_path)
        emit(tree=name, cell='tar', archive_bytes=tar_path.stat().st_size, wall_s=tar_s)
        for cell, args, threads in [
                ('xz lzma2 9e dict256MiB -T1', [xz, '--lzma2=preset=9e,dict=256MiB', '-T1', '-c'], 1),
                ('xz -9e -T0 --block-size=64MiB', [xz, '-9e', '-T0', '--block-size=64MiB', '--memlimit-compress=75%',
                                                   '-c'], 'all')]:
            arc = d / 'tree.tar.xz'
            with open(arc, 'wb') as f:
                m, _ = eb.run(args + [str(tar_path)], stdout=f, pin=threads == 1)
            emit(tree=name, cell='tar | ' + cell, threads=threads, archive_bytes=arc.stat().st_size,
                 compress_wall_s=m['wall_s'], wall_s=round(m['wall_s'] + tar_s, 3), peak_bytes=m['peak_bytes'])
            if threads == 1:
                probe('tar | ' + cell, arc, 'xz')
            arc.unlink()
        tar_path.unlink()
        for cell, flags, threads in [('7z -mx9 -md=256m -mmt1', ['-mx9', '-md=256m', '-mmt1'], 1),
                                     ('7z -mx9 -md=256m (default threads)', ['-mx9', '-md=256m'], 'all')]:
            arc = d / 'tree.7z'
            m, _ = eb.run([z7, 'a', '-t7z', *flags, '-bd', '-bso0', '-bsp0', str(arc.resolve()), '*'], cwd=root,
                          pin=threads == 1)
            emit(tree=name, cell=cell, threads=threads, archive_bytes=arc.stat().st_size, **m)
            if threads == 1:
                probe(cell, arc, '7z')
            arc.unlink()
        shutil.rmtree(d)


if __name__ == '__main__':
    main()
