# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-040: SeedArc recipes against external archivers on the same local dev trees.

  python -I research/scripts/external_baseline.py --tool SEEDARC_APPEND4_STUDY --work EMPTY_DIR \
      --out RESULT_DIR [--sevenzip 7Z] [--xz XZ] [--zstd ZSTD] [--pin-cpu N] TREE_NAME=DIR ...

Rules (fixed before any run):
  * Same machine, same input trees, nothing else measured at the same time; one run per cell.
  * External tools come from PATH (7za, xz, zstd) unless given; their versions are recorded. They use the
    same liblzma / zstd versions as SeedArc's build, so differences come from the container and
    strategy, not the codec library.
  * The tar stage is written by Python's tarfile: PAX format, files in SeedArc's order (UTF-8
    byte order of the relative POSIX path), mtime 0, uid/gid 0, mode 0644, no directories. Its
    time is reported separately and added to the stream archivers' compression time.
  * Size: whole archive file bytes. Time: wall clock of each process (SeedArc's own pack time,
    which excludes its built-in byte-exact verification, is recorded as well). Memory: peak
    working set (Windows) or maximum resident set (Linux, wait4) of the process. With --pin-cpu
    (Linux), single-thread cells run under taskset on that CPU; multi-thread cells are not pinned.
  * Single-thread cells are the comparison; multi-thread cells are reported for speed only.
  * Single-file extraction: three non-empty files at 10% / 50% / 90% of the path order. Stream
    archives (tar.xz, tar.zst) are decompressed through a pipe and read with tarfile only until
    the file is found, then the decompressor is stopped (the best case for a stream). Every
    extracted file is compared with the source by SHA256.
  * Full decode: xz / zstd -dc to a null sink, 7za t, SeedArc verify. SeedArc verify also reads
    and compares the source tree, so it overstates SeedArc's decode time.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

WINDOWS = os.name == 'nt'
if WINDOWS:
    import ctypes.wintypes as wt
PIN = []


class PMC(ctypes.Structure):
    _fields_ = [] if not WINDOWS else [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPagedPoolUsage', ctypes.c_size_t), ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t)]


def peak(proc):
    if not WINDOWS:
        return getattr(proc, 'maxrss', None)
    c = PMC()
    c.cb = ctypes.sizeof(c)
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(wt.HANDLE(int(proc._handle)), ctypes.byref(c), c.cb)
    return c.PeakWorkingSetSize if ok else None


def reap(p):
    """Linux: wait with wait4 to read the child's maximum resident set."""
    _, status, usage = os.wait4(p.pid, 0)
    p.returncode = os.waitstatus_to_exitcode(status)
    p.maxrss = usage.ru_maxrss * 1024


def run(cmd, cwd=None, stdout=None, stdin=None, pin=False):
    cmd = (PIN if pin else []) + list(cmd)
    t = time.perf_counter()
    if WINDOWS:
        p = subprocess.Popen(cmd, cwd=cwd, stdout=stdout or subprocess.DEVNULL, stdin=stdin, stderr=subprocess.PIPE)
        _, err = p.communicate()
    else:
        with open(os.devnull, 'w+b') as null, tempfile.TemporaryFile() as errf:
            p = subprocess.Popen(cmd, cwd=cwd, stdout=stdout or null, stdin=stdin, stderr=errf)
            reap(p)
            errf.seek(0)
            err = errf.read()
    wall = time.perf_counter() - t
    if p.returncode != 0:
        raise SystemExit(f'{cmd[0]} failed ({p.returncode}): {err.decode(errors="replace")[-500:]}')
    return dict(wall_s=round(wall, 3), peak_bytes=peak(p)), err


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def listing(root):
    files = [p for p in root.rglob('*') if p.is_file() and not p.is_symlink()]
    rel = sorted((p.relative_to(root).as_posix() for p in files), key=lambda s: s.encode('utf-8'))
    return rel


def write_tar(root, rel, out):
    t = time.perf_counter()
    with tarfile.open(out, 'w', format=tarfile.PAX_FORMAT, encoding='utf-8') as tf:
        for r in rel:
            info = tarfile.TarInfo(r)
            info.size = (root / r).stat().st_size
            info.mtime, info.uid, info.gid, info.mode, info.uname, info.gname = 0, 0, 0, 0o644, '', ''
            with open(root / r, 'rb') as f:
                tf.addfile(info, f)
    return round(time.perf_counter() - t, 3)


def stream_extract(decoder, archive, member, dest):
    """Decompress through a pipe, read tar headers until `member`, then stop the decoder."""
    t = time.perf_counter()
    p = subprocess.Popen(PIN + decoder + [str(archive)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    found = False
    with tarfile.open(fileobj=p.stdout, mode='r|', encoding='utf-8') as tf:
        for info in tf:
            if info.name == member:
                with tf.extractfile(info) as src, open(dest, 'wb') as out:
                    shutil.copyfileobj(src, out)
                found = True
                break
    p.kill()
    p.stdout.close()
    if WINDOWS:
        p.wait()
    else:
        reap(p)
    wall = time.perf_counter() - t
    if not found:
        raise SystemExit(f'{member} not found in {archive}')
    return dict(wall_s=round(wall, 3), peak_bytes=peak(p))


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--tool', required=True)
    a.add_argument('--work', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('--sevenzip')
    a.add_argument('--xz')
    a.add_argument('--zstd')
    a.add_argument('--pin-cpu', type=int)
    a.add_argument('trees', nargs='+')
    a = a.parse_args()
    tool = str(Path(a.tool).resolve(strict=True))
    work, out = Path(a.work), Path(a.out)
    if work.exists() and any(work.iterdir()):
        raise SystemExit('work dir must be empty')
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    z7 = a.sevenzip or shutil.which('7za')
    xz = a.xz or shutil.which('xz')
    zstd = a.zstd or shutil.which('zstd')
    if a.pin_cpu is not None:
        PIN.extend(['taskset', '-c', str(a.pin_cpu)])
    versions = dict(
        seedarc_tool_sha256=sha(tool),
        sevenzip=next(l for l in subprocess.run([z7, 'i'], capture_output=True, text=True).stdout.splitlines()
                      if '7-Zip' in l).strip(),
        tool_paths=dict(sevenzip=z7, xz=xz, zstd=zstd), pin=PIN,
        xz=subprocess.run([xz, '--version'], capture_output=True, text=True).stdout.strip().replace('\n', '; '),
        zstd=subprocess.run([zstd, '--version'], capture_output=True, text=True).stdout.strip(),
        python=sys.version.split()[0])
    (out / 'versions.json').write_text(json.dumps(versions, indent=1) + '\n', encoding='utf-8')
    results = open(out / 'raw.jsonl', 'a', encoding='utf-8')

    def emit(**row):
        results.write(json.dumps(row, ensure_ascii=False) + '\n')
        results.flush()
        print(json.dumps(row, ensure_ascii=False), flush=True)

    for spec in a.trees:
        name, path = spec.split('=', 1)
        root = Path(path).resolve(strict=True)
        rel = listing(root)
        total = sum((root / r).stat().st_size for r in rel)
        picks = []
        for frac in (0.1, 0.5, 0.9):
            i = int(len(rel) * frac)
            while (root / rel[i]).stat().st_size == 0:
                i += 1
            picks.append(rel[i])
        d = work / name
        d.mkdir()
        tar_path = d / 'tree.tar'
        tar_s = write_tar(root, rel, tar_path)
        emit(tree=name, cell='tar', files=len(rel), input_bytes=total, archive_bytes=tar_path.stat().st_size,
             wall_s=tar_s, picks=picks)
        def probe(cell, arc, kind):
            """Full decode and single-file extraction right after writing; then the archive goes."""
            if kind == 'seedarc':
                m, _ = run([tool, 'verify', str(arc), str(root)], pin=True)
            elif kind == '7z':
                m, _ = run([z7, 't', '-bd', '-bso0', '-bsp0', str(arc)], pin=True)
            else:
                m, _ = run(([xz] if kind == 'xz' else [zstd, '--long=27']) + ['-dc', str(arc)], pin=True)
            emit(tree=name, cell=cell, op='full decode', **m)
            for k, member in enumerate(picks):
                dest = d / f'pick{k}'
                if dest.exists():
                    shutil.rmtree(dest) if dest.is_dir() else dest.unlink()
                if kind == 'seedarc':
                    dest.mkdir()
                    m, _ = run([tool, 'extract', str(arc), member, str(dest), 'out'], pin=True)
                    got = dest / 'out'
                elif kind == '7z':
                    dest.mkdir()
                    # A list file: 7z reads an argument starting with '@' as one, and -spd turns
                    # off wildcards, so any path (e.g. npm's @scope/...) is taken literally.
                    lst = d / 'member.lst'
                    lst.write_text(member + '\n', encoding='utf-8')
                    m, _ = run([z7, 'e', '-bd', '-bso0', '-bsp0', '-spd', '-scsUTF-8', str(arc), f'-o{dest}',
                                f'@{lst}'], pin=True)
                    got = dest / Path(member).name
                else:
                    m = stream_extract([xz, '-dc'] if kind == 'xz' else [zstd, '--long=27', '-dc'], arc, member, dest)
                    got = dest
                ok = sha(got) == sha(root / member)
                emit(tree=name, cell=cell, op=f'extract {int((0.1, 0.5, 0.9)[k] * 100)}%', ok=ok, **m)
                if not ok:
                    raise SystemExit(f'extracted {member} differs ({cell})')
            arc.unlink()

        for recipe in ['solid/zstd-19/g1-r0/frame-32MiB', 'solid/lzma2-9e/g1-r0/frame-32MiB',
                       'solid/lzma2-9e/g1-r0/frame-32MiB/dedup', 'solid/lzma2-9e/g1-r0/frame-whole/dedup']:
            arc = d / (recipe.replace('/', '_') + '.sadx')
            with open(d / 'pack.json', 'wb') as f:
                m, err = run([tool, '--jobs', '1', 'recipe-pack', 'seed', recipe, str(root), str(arc)], stdout=f,
                             pin=True)
            info = json.loads((d / 'pack.json').read_text(encoding='utf-8'))
            emit(tree=name, cell='seedarc ' + recipe, threads=1, archive_bytes=arc.stat().st_size,
                 pack_wall_s=round(info['pack_wall_ms'] / 1000, 3), **m)
            probe('seedarc ' + recipe, arc, 'seedarc')
        arc = d / 'auto.sadx'
        with open(d / 'pack.json', 'wb') as f:
            m, err = run([tool, '--jobs', 'auto', 'recipe-pack', 'seed', 'solid/lzma2-9e/g1-r0/frame-32MiB/dedup',
                          str(root), str(arc)], stdout=f)
        info = json.loads((d / 'pack.json').read_text(encoding='utf-8'))
        emit(tree=name, cell='seedarc solid/lzma2-9e/g1-r0/frame-32MiB/dedup', threads='auto',
             archive_bytes=arc.stat().st_size, pack_wall_s=round(info['pack_wall_ms'] / 1000, 3),
             jobs_auto=json.loads(err.decode().strip().splitlines()[-1])['jobs_auto'], **m)
        arc.unlink()
        for cell, args, ext, threads in [('xz -9e -T1', [xz, '-9e', '-T1', '-c'], 'xz', 1),
                                         ('xz -9e -T0', [xz, '-9e', '-T0', '-c'], 'xz', 'all'),
                                         ('zstd -19 -T1', [zstd, '-19', '-T1', '-c'], 'zst', 1),
                                         ('zstd -19 --long=27 -T1', [zstd, '-19', '--long=27', '-T1', '-c'], 'zst', 1),
                                         ('zstd -19 -T0', [zstd, '-19', '-T0', '-c'], 'zst', 'all')]:
            arc = d / f"tree.{cell.replace(' ', '').replace('=', '')}.tar.{ext}"
            with open(arc, 'wb') as f:
                m, _ = run(args + [str(tar_path)], stdout=f, pin=threads == 1)
            emit(tree=name, cell='tar | ' + cell, threads=threads, archive_bytes=arc.stat().st_size,
                 compress_wall_s=m['wall_s'], wall_s=round(m['wall_s'] + tar_s, 3), peak_bytes=m['peak_bytes'])
            if threads == 1:
                probe('tar | ' + cell, arc, ext)
            else:
                arc.unlink()
        tar_path.unlink()  # 7z reads the tree itself
        for cell, flags, threads in [('7z -mx9 -mmt1', ['-mx9', '-mmt1'], 1),
                                     ('7z -mx9 -ms=32m -mmt1', ['-mx9', '-ms=32m', '-mmt1'], 1),
                                     ('7z -mx9 (default threads)', ['-mx9'], 'all')]:
            arc = d / f"tree-{flags[-1].strip('-=')}-{threads}.7z"
            m, _ = run([z7, 'a', '-t7z', *flags, '-bd', '-bso0', '-bsp0', str(arc.resolve()), '*'], cwd=root,
                       pin=threads == 1)
            emit(tree=name, cell=cell, threads=threads, archive_bytes=arc.stat().st_size, **m)
            if threads == 1:
                probe(cell, arc, '7z')
            else:
                arc.unlink()
        shutil.rmtree(d)


if __name__ == '__main__':
    main()
