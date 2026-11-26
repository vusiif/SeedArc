# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-031 India side: fixed-toolchain Release build + full CTest + writer regression check, then
package the tool with the toolchain runtime it links, build the shared data subset, and run
cross_cpu_check.py from the unpacked package (the exact bytes another host will run).

Results: xcpu-tool.tar.gz, xcpu-data.tar.xz, xcpu-vps-in1.json, bundle-hashes.json.
"""
import hashlib
import json
import os
import platform
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

from bounded_expand_dev import load, remove_owned, sha, write_json

work = Path(os.environ['TASK_WORK']).resolve()
results = Path(os.environ['TASK_RESULTS']).resolve()
started = time.monotonic()
prior = Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')
linux_src = Path('/home/sc/long-tasks/20261124T051435Z-cef36683/work/data')
silesia_zip = Path('/home/sc/long-tasks/20261124T102611Z-2912f5b3/results/inputs/silesia-dev.zip')
SILESIA_ZIP_SHA = '7f7efa49d4d71a6af5fc8bb295ea6696ea05a908fcc80fa724168bfb22010f32'
VCPKG_STATUS_SHA = 'c894d1796de7f727ca1a2fc680a9f5324a067da90a6a4b68f8c6233687914bec'
LINUX_BUDGET = 40 * 1024**2
SILESIA_COUNT = 3  # the smallest members of the fixed dev zip (deterministic: its SHA256 is checked)

toolchain_lib = prior / 'toolchain/usr/lib/x86_64-linux-gnu'
env = os.environ.copy()
env.update(PATH=str(prior / 'toolchain/usr/bin') + ':' + env['PATH'], LD_LIBRARY_PATH=str(toolchain_lib),
           LC_ALL='C.UTF-8')
os.environ.update(LC_ALL='C.UTF-8')


def note(phase):
    write_json(results / 'progress.json', dict(phase=phase, elapsed_s=time.monotonic() - started))


def run(argv, label, timeout=1800, **kw):
    print(json.dumps(dict(step=label, argv=list(map(str, argv)))), flush=True)
    with (results / (label + '.log')).open('w') as stream:
        proc = subprocess.run(list(map(str, argv)), env=kw.get('env', env), stdout=stream, stderr=subprocess.STDOUT,
                              timeout=timeout)
    if proc.returncode:
        raise RuntimeError(label + ' failed; preserve evidence')


inputs = results / 'inputs'
inputs.mkdir()
for p in work.iterdir():
    if p.is_file():
        shutil.copyfile(p, inputs / p.name)
if sha(prior / 'build/vcpkg_installed/vcpkg/status') != VCPKG_STATUS_SHA:
    raise ValueError('fixed dependency status changed')
if silesia_zip.is_symlink() or sha(silesia_zip) != SILESIA_ZIP_SHA:
    raise ValueError('Silesia dev zip changed')
source = work / 'source'
source.mkdir()
with zipfile.ZipFile(work / 'source.zip') as z:
    for entry in z.infolist():
        p = PurePosixPath(entry.filename)
        if p.is_absolute() or '..' in p.parts or '\\' in entry.filename:
            raise ValueError('unsafe source entry')
        z.extract(entry, source)
note('build')
base = ['-G', 'Ninja', '-DCMAKE_CXX_COMPILER=' + str(prior / 'toolchain/usr/bin/g++-14'),
        '-DCMAKE_PREFIX_PATH=' + str(prior / 'build/vcpkg_installed/x64-linux'),
        '-DSEEDARC_BUILD_RESEARCH_TOOLS=ON', '-DBUILD_TESTING=ON']
release = work / 'release'
run(['cmake', '-S', source, '-B', release, '-DCMAKE_BUILD_TYPE=Release', *base], 'release-configure')
run(['cmake', '--build', release, '--parallel', str(os.cpu_count() or 4)], 'release-build')
run(['ctest', '--test-dir', release, '--output-on-failure', '--parallel', '1'], 'release-ctest')
shutil.copyfile(release / 'CMakeCache.txt', results / 'release-cache.txt')
tool = release / 'research/tools/seedarc_append4_study'
run(['cmake', '-DTOOL=' + str(tool), '-DWORK=' + str(work / 'bounded-cross'),
     '-DREPORT=' + str(results / 'linux-cross-hashes.json'), '-P',
     source / 'research/tools/check_bounded_solid.cmake'], 'bounded-cross')
if json.loads((results / 'linux-cross-hashes.json').read_text()) != json.loads((work / 'cross-reference.json').read_text()):
    raise ValueError('writer output changed: portable fixture archive hashes differ')
remove_owned(work / 'bounded-cross', work)

note('package tool')
# Ship every library ldd resolves inside the fixed toolchain; system libraries (glibc) stay the host's.
ldd = subprocess.run(['ldd', str(tool)], env=env, capture_output=True, text=True, check=True).stdout
shipped = []
for line in ldd.splitlines():
    parts = line.split('=>')
    if len(parts) == 2 and parts[1].strip().startswith(str(prior)):
        shipped.append(Path(parts[1].split('(')[0].strip()))
pkg = work / 'xcpu-tool'
(pkg / 'lib').mkdir(parents=True)
shutil.copyfile(tool, pkg / 'seedarc_append4_study')
(pkg / 'seedarc_append4_study').chmod(0o755)
for lib in shipped:
    shutil.copyfile(lib, pkg / 'lib' / lib.name)
for name in ['cross_cpu_check.py', 'bounded_fixture.py', 'recipe_fixture.py']:
    shutil.copyfile(work / name, pkg / name)
write_json(pkg / 'manifest.json', dict(ldd=ldd, shipped=[p.name for p in shipped],
    files={p.relative_to(pkg).as_posix(): sha(p) for p in sorted(pkg.rglob('*')) if p.is_file()}))
with tarfile.open(results / 'xcpu-tool.tar.gz', 'w:gz') as tar:
    tar.add(pkg, arcname='xcpu-tool')

note('data subset')
data = work / 'xdata'
rows = [r for r in load(linux_src, work / 'source.tsv', verify_content=False) if r['path'].startswith('linux/')]
rows.sort(key=lambda r: hashlib.sha256(('dev031-subset:' + r['path']).encode()).hexdigest())
total = 0
for r in rows:
    if total + r['size'] > LINUX_BUDGET:
        continue
    src = linux_src / r['path']
    if src.is_symlink() or not src.is_file() or src.stat().st_size != r['size'] or sha(src) != r['sha256']:
        raise ValueError('linux input differs from the DEV-023 manifest: ' + r['path'])
    dest = data / r['path']
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
    total += r['size']
with zipfile.ZipFile(silesia_zip) as z:
    members = sorted((i for i in z.infolist() if i.filename.startswith('silesia/') and not i.is_dir()),
                     key=lambda i: (i.file_size, i.filename))[:SILESIA_COUNT]
    if len(members) != SILESIA_COUNT:
        raise ValueError('Silesia dev zip holds fewer members than expected')
    for name in (i.filename for i in members):
        if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
            raise ValueError('unsafe Silesia member')
        dest = data / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        with z.open(name) as s, dest.open('xb') as d:
            shutil.copyfileobj(s, d)
with tarfile.open(results / 'xcpu-data.tar.xz', 'w:xz', preset=9) as tar:
    tar.add(data, arcname='xdata')

note('check from the unpacked package')
unpacked = work / 'unpacked'
unpacked.mkdir()
for archive in ['xcpu-tool.tar.gz', 'xcpu-data.tar.xz']:
    with tarfile.open(results / archive) as tar:
        tar.extractall(unpacked, filter='data')
check_env = dict(os.environ, LD_LIBRARY_PATH=str(unpacked / 'xcpu-tool/lib'), LC_ALL='C.UTF-8')
run([sys.executable, '-I', unpacked / 'xcpu-tool/cross_cpu_check.py', 'run', unpacked / 'xcpu-tool/seedarc_append4_study',
     unpacked / 'xdata', work / 'xcpu-work', results / 'xcpu-vps-in1.json'], 'cross-cpu-check', timeout=7200, env=check_env)
write_json(results / 'bundle-hashes.json', {n: sha(results / n) for n in ['xcpu-tool.tar.gz', 'xcpu-data.tar.xz']}
           | dict(linux_files=len(list((data / 'linux').rglob('*'))), linux_bytes=total,
                silesia=[i.filename for i in members]))
write_json(results / 'environment.json', dict(platform=platform.platform(), python=sys.version,
    cpu_count=os.cpu_count(), cpu_info=Path('/proc/cpuinfo').read_text(), memory_info=Path('/proc/meminfo').read_text(),
    tool_sha256=sha(tool), source_zip_sha256=sha(work / 'source.zip')))
write_json(results / 'task-complete.json', dict(elapsed_s=time.monotonic() - started))
note('complete')
