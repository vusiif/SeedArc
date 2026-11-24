"""Build a task-local, manifest-pinned Release environment; never run apt install."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import zipfile

work = Path(os.environ['TASK_WORK']).resolve()
results = Path(os.environ['TASK_RESULTS']).resolve()
os.chdir(work)


def run(args, name, env=None):
    print(json.dumps(dict(step=name, argv=[str(a) for a in args])), flush=True)
    with (results / (name + '.log')).open('w') as log:
        p = subprocess.run([str(a) for a in args], stdout=log, stderr=subprocess.STDOUT,
                           env=env, timeout=3600)
    if p.returncode:
        raise RuntimeError(f'{name}: exit {p.returncode}; see {name}.log')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


packages = work / 'packages'
packages.mkdir()
toolchain = work / 'toolchain'
toolchain.mkdir()
specs = ['g++-14', 'cmake', 'ninja-build', 'pkgconf', 'zip', 'unzip']
simulation = subprocess.run(['apt-get', '-s', '--no-install-recommends', 'install', *specs],
                            capture_output=True, text=True, check=True)
(results / 'apt-resolution.txt').write_text(simulation.stdout)
resolved = re.findall(r'^Inst (\S+)(?: \[[^\]]+\])? \((\S+)', simulation.stdout, re.M)
if not resolved or any(p.startswith(('libc6', 'linux-image', 'systemd')) for p, _ in resolved):
    raise RuntimeError('unexpected toolchain resolution; do not modify host or replace libc')
records = []
for index, (package, version) in enumerate(resolved):
    os.chdir(packages)
    run(['apt-get', 'download', f'{package}={version}'], f'apt-{index:02d}')
    new = list(packages.glob('*.deb'))
    if len(new) != 1:
        raise RuntimeError('unexpected downloaded package set')
    deb = new[0]
    metadata = subprocess.run(['apt-cache', 'show', f'{package}={version}'],
                              capture_output=True, text=True, check=True).stdout
    expected = re.search(r'^SHA256: (\w+)$', metadata, re.M)
    actual = digest(deb)
    if not expected or actual != expected[1]:
        raise ValueError('deb hash differs from trusted host package index')
    records.append(dict(package=package, version=version, sha256=actual))
    run(['dpkg-deb', '-x', deb, toolchain], f'extract-{index:02d}')
    deb.unlink()
(results / 'toolchain-packages.json').write_text(json.dumps(records, indent=2))
os.chdir(work)
env = os.environ.copy()
env.update(PATH=str(toolchain / 'usr/bin') + ':' + env['PATH'],
           LD_LIBRARY_PATH=str(toolchain / 'usr/lib/x86_64-linux-gnu'),
           XDG_CACHE_HOME=str(work / 'cache'), VCPKG_DOWNLOADS=str(work / 'vcpkg-downloads'),
           VCPKG_MAX_CONCURRENCY='2', VCPKG_BINARY_SOURCES='clear',
           VCPKG_FORCE_SYSTEM_BINARIES='1',
           CC=str(toolchain / 'usr/bin/gcc-14'), CXX=str(toolchain / 'usr/bin/g++-14'))
for name in ['cache', 'vcpkg-downloads']:
    (work / name).mkdir()
source = work / 'source'
source.mkdir()
with zipfile.ZipFile(work / 'source.zip') as archive:
    for member in archive.infolist():
        path = Path(member.filename)
        if path.is_absolute() or '..' in path.parts:
            raise ValueError('unsafe source archive')
        archive.extract(member, source)
run(['git', 'init', work / 'vcpkg'], 'vcpkg-init', env)
run(['git', '-C', work / 'vcpkg', 'remote', 'add', 'origin', 'https://github.com/microsoft/vcpkg.git'], 'vcpkg-origin', env)
baseline = json.loads((source / 'vcpkg-configuration.json').read_text())['default-registry']['baseline']
run(['git', '-C', work / 'vcpkg', 'fetch', '--depth=1', 'origin', baseline], 'vcpkg-fetch', env)
run(['git', '-C', work / 'vcpkg', 'checkout', '--detach', 'FETCH_HEAD'], 'vcpkg-checkout', env)
env['VCPKG_ROOT'] = str(work / 'vcpkg')
run(['bash', work / 'vcpkg/bootstrap-vcpkg.sh', '-disableMetrics'], 'vcpkg-bootstrap', env)
build = work / 'build'
run(['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
     '-DCMAKE_C_COMPILER=' + env['CC'], '-DCMAKE_CXX_COMPILER=' + env['CXX'],
     '-DCMAKE_TOOLCHAIN_FILE=' + str(work / 'vcpkg/scripts/buildsystems/vcpkg.cmake'),
     '-DVCPKG_TARGET_TRIPLET=x64-linux', '-DVCPKG_HOST_TRIPLET=x64-linux',
     '-DSEEDARC_BUILD_RESEARCH_TOOLS=ON', '-DBUILD_TESTING=ON'], 'configure', env)
run(['cmake', '--build', build, '--parallel', '2'], 'build', env)
run(['ctest', '--test-dir', build, '--output-on-failure', '--parallel', '1'], 'ctest', env)
shutil.copyfile(build / 'CMakeCache.txt', results / 'CMakeCache.txt')
shutil.copyfile(build / 'vcpkg_installed/vcpkg/status', results / 'vcpkg-status.txt')
for executable in ['g++-14', 'cmake', 'ninja']:
    run([executable, '--version'], 'version-' + executable, env)
(results / 'source-sha256.txt').write_text(digest(work / 'source.zip') + '\n')
run([sys.executable, work / 'append_india_dev.py', '--input', work, '--work', work,
     '--results', results, '--tool', build / 'research/tools/seedarc_append4_study',
     '--cli', build / 'cli/seedarc'], 'dev-pilot', env)
