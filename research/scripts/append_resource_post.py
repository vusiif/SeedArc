# SPDX-License-Identifier: GPL-3.0-or-later
"""Build/validate the resource patch with the existing fixed India toolchain."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

work = Path(os.environ['TASK_WORK']).resolve()
results = Path(os.environ['TASK_RESULTS']).resolve()
prior = Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')
env = os.environ.copy()
env['PATH'] = str(prior / 'toolchain/usr/bin') + ':' + env['PATH']
env['LD_LIBRARY_PATH'] = str(prior / 'toolchain/usr/lib/x86_64-linux-gnu')
env['LC_ALL'] = 'C.UTF-8'
compiler = prior / 'toolchain/usr/bin/g++-14'
prefix = prior / 'build/vcpkg_installed/x64-linux'
status = (prior / 'build/vcpkg_installed/vcpkg/status').read_text()
packages = {}
for paragraph in status.split('\n\n'):
    fields = dict(line.split(': ', 1) for line in paragraph.splitlines() if ': ' in line)
    if fields.get('Architecture') == 'x64-linux':
        packages[fields.get('Package')] = fields.get('Version')
if packages.get('zstd') != '1.5.7' or packages.get('liblzma') != '5.8.4':
    raise RuntimeError('fixed manifest dependencies changed')
(results / 'dependency-status.txt').write_text(status)
source = work / 'source'
source.mkdir()
with zipfile.ZipFile(work / 'source.zip') as archive:
    for member in archive.infolist():
        path = Path(member.filename)
        if path.is_absolute() or '..' in path.parts:
            raise ValueError('unsafe source member')
        archive.extract(member, source)
inputs = results / 'inputs'
inputs.mkdir()
for name in ['source.zip', 'source-patch.diff', 'source-hashes.json',
             'append_resource_post.py', 'append_resource_probe.py',
             'append_history_r2.py', 'check_append_history_r2.py']:
    shutil.copyfile(work / name, inputs / name)


def run(args, name):
    print(json.dumps(dict(step=name, argv=list(map(str, args)))), flush=True)
    with (results / (name + '.log')).open('w') as stream:
        process = subprocess.run(list(map(str, args)), stdout=stream, stderr=subprocess.STDOUT,
                                 env=env, timeout=1800)
    if process.returncode:
        raise RuntimeError(f'{name} failed: {process.returncode}; evidence retained')


base = ['-G', 'Ninja', '-DCMAKE_CXX_COMPILER=' + str(compiler),
        '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DSEEDARC_BUILD_RESEARCH_TOOLS=ON',
        '-DBUILD_TESTING=ON']
release = work / 'release'
run(['cmake', '-S', source, '-B', release, '-DCMAKE_BUILD_TYPE=Release', *base], 'release-configure')
run(['cmake', '--build', release, '--parallel', '2'], 'release-build')
run(['ctest', '--test-dir', release, '--output-on-failure', '--parallel', '1'], 'release-ctest')
shutil.copyfile(release / 'CMakeCache.txt', results / 'release-cache.txt')
run([sys.executable, work / 'check_append_history_r2.py', '--tool',
     release / 'research/tools/seedarc_append4_study', '--work', results / 'history'], 'history')
asan = work / 'asan'
run(['cmake', '-S', source, '-B', asan, '-DCMAKE_BUILD_TYPE=Debug',
     '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer',
     '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined', *base], 'asan-configure')
run(['cmake', '--build', asan, '--parallel', '2'], 'asan-build')
run(['ctest', '--test-dir', asan, '--output-on-failure', '--parallel', '1'], 'asan-ctest')
shutil.copyfile(asan / 'CMakeCache.txt', results / 'asan-cache.txt')
shutil.copyfile(asan / 'Testing/Temporary/LastTest.log', results / 'asan-last-test.log')
if asan.is_symlink() or asan.resolve().parent != work:
    raise RuntimeError('unexpected sanitizer build root')
# Build evidence is saved; remove only reproducible objects before the disk-capacity probe.
shutil.rmtree(asan)
run([sys.executable, work / 'append_resource_probe.py', '--tool',
     release / 'research/tools/seedarc_append4_study', '--work', work / 'probe',
     '--results', results / 'after'], 'after-probe')
for executable in ['g++-14', 'cmake', 'ninja']:
    run([executable, '--version'], 'version-' + executable)
(results / 'post-complete.json').write_text(json.dumps(dict(status='validated resource probe complete',
    binary_sha256=hashlib.sha256((release / 'research/tools/seedarc_append4_study').read_bytes()).hexdigest(),
    source_archive_sha256=hashlib.sha256((work / 'source.zip').read_bytes()).hexdigest(),
    scope='capacity, compatibility and safety checks; not Seed compression benefit'), indent=2))
