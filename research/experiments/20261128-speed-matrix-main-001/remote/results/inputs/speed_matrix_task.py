# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-045 India task: the speed matrix (speed_matrix.py) on the DEV-040 dev trees.

1. Fixed-toolchain Release build of SeedArc from source.zip (commit 0acaa4b, DEV-044) + full CTest
   + writer cross-check against the MSVC hashes of the same commit (cross-reference.json; the
   default zstd trailer of DEV-044 changed these hashes, `--trailer raw` still gives DEV-040's).
2. zstd 1.5.7 and xz 5.8.4 tools from the same vcpkg sources as SeedArc's libraries (as DEV-040);
   7-Zip 23.01 from Ubuntu's package, unpacked into the task only. DEV-042: 23.01 with -md=256m
   writes the same bytes as 26.00, so the 256 MiB cells stand for current 7-Zip.
3. Data: the DEV-040 copy already on the server (/home/sc/seedarc-data/dev040, read only, SHA
   checked file by file); nothing new is uploaded. One tree group at a time is unpacked, measured
   and removed (2 GiB task disk budget).
"""
import json
import os
import platform
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import time
import zipfile

from bounded_expand_dev import remove_owned, sha, write_json

if sys.argv[1:]:
    raise SystemExit('usage: speed_matrix_task.py')
work = Path(os.environ['TASK_WORK']).resolve()
results = Path(os.environ['TASK_RESULTS']).resolve()
started = time.monotonic()
prior = Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')
data_dir = Path('/home/sc/seedarc-data/dev040')
VCPKG_STATUS_SHA = 'c894d1796de7f727ca1a2fc680a9f5324a067da90a6a4b68f8c6233687914bec'
DATA_SHA = '145b30a683de5e478de3a34a63541d2e1a84e3f869bd8779eb2bc9bcbb86e10e'
MANIFEST_SHA = 'a9dbb0a580a75410baf4c171bd5011cf4465274b190faeacc0810205844e8f80'

env = os.environ.copy()
env.update(PATH=str(prior / 'toolchain/usr/bin') + ':' + env['PATH'],
           LD_LIBRARY_PATH=str(prior / 'toolchain/usr/lib/x86_64-linux-gnu'), LC_ALL='C.UTF-8')
os.environ.update(LD_LIBRARY_PATH=env['LD_LIBRARY_PATH'], LC_ALL=env['LC_ALL'], PATH=env['PATH'])
progress = dict(phase='start')


def note(**kw):
    progress.update(kw, elapsed_s=round(time.monotonic() - started, 1))
    write_json(results / 'progress.json', progress)


def run(argv, label, timeout=3600, cwd=None):
    print(json.dumps(dict(step=label, argv=list(map(str, argv)))), flush=True)
    with (results / (label + '.log')).open('w') as stream:
        proc = subprocess.run(list(map(str, argv)), env=env, stdout=stream, stderr=subprocess.STDOUT,
                              timeout=timeout, cwd=cwd)
    if proc.returncode:
        raise RuntimeError(label + ' failed; preserve evidence')


inputs = results / 'inputs'
inputs.mkdir()
for p in work.iterdir():
    if p.is_file():
        shutil.copyfile(p, inputs / p.name)
if sha(prior / 'build/vcpkg_installed/vcpkg/status') != VCPKG_STATUS_SHA:
    raise ValueError('fixed dependency status changed')
# Data first: a changed copy should stop the task before the long build.
if sha(data_dir / 'dev-trees.tar.zst') != DATA_SHA or sha(data_dir / 'dev-trees.tar.manifest.tsv') != MANIFEST_SHA:
    raise ValueError('uploaded data changed')
source = work / 'source'
source.mkdir()
with zipfile.ZipFile(work / 'source.zip') as z:
    for entry in z.infolist():
        p = PurePosixPath(entry.filename)
        if p.is_absolute() or '..' in p.parts or '\\' in entry.filename:
            raise ValueError('unsafe source entry')
        z.extract(entry, source)

# 1. SeedArc.
note(phase='build seedarc')
gxx = prior / 'toolchain/usr/bin/g++-14'
gcc = next((p for p in [prior / 'toolchain/usr/bin/gcc-14', prior / 'toolchain/usr/bin/x86_64-linux-gnu-gcc-14']
            if p.exists()), None)
if gcc is None:
    raise FileNotFoundError('no gcc-14 in the fixed toolchain')
bin_dir = work / 'bin'
tool = bin_dir / 'seedarc_append4_study'
base = ['-G', 'Ninja', '-DCMAKE_CXX_COMPILER=' + str(gxx),
        '-DCMAKE_PREFIX_PATH=' + str(prior / 'build/vcpkg_installed/x64-linux'),
        '-DSEEDARC_BUILD_RESEARCH_TOOLS=ON', '-DBUILD_TESTING=ON']
release = work / 'release'
run(['cmake', '-S', source, '-B', release, '-DCMAKE_BUILD_TYPE=Release', *base], 'release-configure')
run(['cmake', '--build', release, '--parallel', str(os.cpu_count() or 4)], 'release-build')
note(phase='ctest')
run(['ctest', '--test-dir', release, '--output-on-failure', '--parallel', '1'], 'release-ctest', timeout=3 * 3600)
shutil.copyfile(release / 'CMakeCache.txt', results / 'release-cache.txt')
bin_dir.mkdir()
shutil.copy2(release / 'research/tools/seedarc_append4_study', tool)
run(['cmake', '-DTOOL=' + str(tool), '-DWORK=' + str(work / 'bounded-cross'),
     '-DREPORT=' + str(results / 'linux-cross-hashes.json'), '-P',
     source / 'research/tools/check_bounded_solid.cmake'], 'bounded-cross')
if json.loads((results / 'linux-cross-hashes.json').read_text()) != json.loads((work / 'cross-reference.json').read_text()):
    raise ValueError('writer output differs from MSVC at the same commit')
remove_owned(work / 'bounded-cross', work)
remove_owned(release, work)  # disk budget: only the copied tool is needed from here on

# 2. External tools.
note(phase='build external tools')
zsrc = next((prior / 'vcpkg/buildtrees/zstd/src').glob('v1.5.7-*'))
xsrc = next((prior / 'vcpkg/buildtrees/liblzma/src').glob('v5.8.4-*'))
zbuild = work / 'zstd-build'
run(['cmake', '-S', zsrc / 'build/cmake', '-B', zbuild, '-G', 'Ninja', '-DCMAKE_C_COMPILER=' + str(gcc),
     '-DCMAKE_CXX_COMPILER=' + str(gxx), '-DCMAKE_BUILD_TYPE=Release', '-DZSTD_BUILD_PROGRAMS=ON',
     '-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_STATIC=ON', '-DZSTD_BUILD_TESTS=OFF', '-DZSTD_PROGRAMS_LINK_SHARED=OFF',
     '-DZSTD_ZLIB_SUPPORT=OFF', '-DZSTD_LZMA_SUPPORT=OFF', '-DZSTD_LZ4_SUPPORT=OFF'], 'zstd-configure')
run(['cmake', '--build', zbuild, '--target', 'zstd', '--parallel', str(os.cpu_count() or 4)], 'zstd-build')
run(['cmake', '-S', xsrc, '-B', work / 'xz-build', '-G', 'Ninja', '-DCMAKE_C_COMPILER=' + str(gcc),
     '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TOOLS=ON', '-DXZ_TOOL_XZ=ON',
     '-DXZ_NLS=OFF', '-DXZ_DOXYGEN=OFF'], 'xz-configure')  # BUILD_TOOLS: vcpkg's build-tools.patch
run(['cmake', '--build', work / 'xz-build', '--target', 'xz', '--parallel', str(os.cpu_count() or 4)], 'xz-build')
zstd = bin_dir / 'zstd'
xz = bin_dir / 'xz'
shutil.copy2(next(p for p in zbuild.rglob('zstd') if p.is_file() and os.access(p, os.X_OK)), zstd)
shutil.copy2(next(p for p in (work / 'xz-build').rglob('xz') if p.is_file() and os.access(p, os.X_OK)), xz)
remove_owned(zbuild, work)
remove_owned(work / 'xz-build', work)
pkg = work / '7zip-deb'
pkg.mkdir()
# Non-ESM version (DEV-040 attempt 2: the ESM build needs credentials this task cannot read).
madison = subprocess.run(['apt-cache', 'madison', '7zip'], capture_output=True, text=True, env=env, check=True).stdout
versions = [line.split('|')[1].strip() for line in madison.splitlines()
            if line.count('|') >= 2 and 'esm.ubuntu.com' not in line]
if not versions:
    raise RuntimeError('no non-ESM 7zip version: ' + madison)
(results / 'apt-madison-7zip.txt').write_text(madison)
run(['apt-get', 'download', '7zip=' + versions[0]], 'apt-download-7zip', cwd=pkg)
deb = next(pkg.glob('7zip_*.deb'))
run(['dpkg-deb', '-x', deb, pkg / 'root'], 'dpkg-extract-7zip')
sevenzip = next(p for p in [pkg / 'root/usr/lib/7zip/7zz', pkg / 'root/usr/lib/7zip/7z', pkg / 'root/usr/bin/7zz']
                if p.exists() and not p.is_symlink())
write_json(results / 'environment.json', dict(platform=platform.platform(), python=sys.version,
    cpu_count=os.cpu_count(), affinity=sorted(os.sched_getaffinity(0)),
    cpu_info=Path('/proc/cpuinfo').read_text(), memory_info=Path('/proc/meminfo').read_text(),
    tool_sha256=sha(tool), zstd_sha256=sha(zstd), xz_sha256=sha(xz), sevenzip_deb=deb.name,
    sevenzip_deb_sha256=sha(deb), sevenzip_sha256=sha(sevenzip), zstd_source=zsrc.name, xz_source=xsrc.name,
    source_zip_sha256=sha(work / 'source.zip')))

# 3. Data and measurement.
manifest = {}
for line in (data_dir / 'dev-trees.tar.manifest.tsv').read_text(encoding='utf-8').splitlines():
    name, size, digest = line.split('\t')
    manifest[name] = (int(size), digest)


def unpack(prefix):
    target = work / 'data'
    target.mkdir(exist_ok=True)
    with subprocess.Popen([str(zstd), '-dc', '--long=27', str(data_dir / 'dev-trees.tar.zst')],
                          stdout=subprocess.PIPE) as dec:
        proc = subprocess.run(['tar', '-x', '-C', str(target), '--no-same-owner', '--', prefix],
                              stdin=dec.stdout, env=env)
        dec.stdout.close()
    if proc.returncode or dec.returncode:
        raise RuntimeError('unpack failed for ' + prefix)
    want = {k: v for k, v in manifest.items() if k.startswith(prefix + '/')}
    got = [p for p in (target / prefix).rglob('*') if p.is_file()]
    if len(got) != len(want):
        raise ValueError(f'{prefix}: {len(got)} files, manifest {len(want)}')
    for p in got:
        rel = p.relative_to(target).as_posix()
        if p.is_symlink() or rel not in want or p.stat().st_size != want[rel][0] or sha(p) != want[rel][1]:
            raise ValueError('unpacked file differs: ' + rel)
    return target / prefix


def measure(trees, round_name):
    note(phase='measure ' + round_name)
    run([sys.executable, '-I', work / 'speed_matrix.py', '--tool', tool, '--work', work / ('sw-' + round_name),
         '--out', results / 'speed', '--sevenzip', sevenzip, '--xz', xz, '--pin-cpu', '0',
         *[f'{n}={p}' for n, p in trees]], 'speed-' + round_name, timeout=10 * 3600)


note(phase='unpack append-r1-dev')
dev = unpack('append-r1-dev')
measure([('append-r1-dev', dev)], 'append-r1-dev')
remove_owned(dev, work / 'data')
note(phase='unpack dup-local-dev-r1')
dup = unpack('dup-local-dev-r1')
measure([('cargo', dup / 'cargo'), ('rustup', dup / 'rustup'), ('all', dup)], 'dup-local')
remove_owned(dup, work / 'data')
write_json(results / 'task-complete.json', dict(elapsed_s=round(time.monotonic() - started, 1)))
note(phase='complete')
