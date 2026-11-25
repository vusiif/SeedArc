# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-023 India task: fixed toolchain Release build + CTest, cross-platform checks, then the
four pre-registered units in order (source-added10 first), auditing each before the next."""
import json
import os
import platform
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import time
import zipfile

from bounded_expand_dev import load, remove_owned, run_unit, sha, write_json
import bounded_expand_report as report

work = Path(os.environ['TASK_WORK']).resolve()
results = Path(os.environ['TASK_RESULTS']).resolve()
task_root = work.parent
started = time.monotonic()
BUDGET_S = 90 * 60
prior = Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')   # fixed toolchain, vcpkg, enwik8
linux_src = Path('/home/sc/long-tasks/20261124T051435Z-cef36683/work/data')  # Linux 6.12 512 MiB dev
silesia_zip = Path('/home/sc/long-tasks/20261124T102611Z-2912f5b3/results/inputs/silesia-dev.zip')
SILESIA_ZIP_SHA = '7f7efa49d4d71a6af5fc8bb295ea6696ea05a908fcc80fa724168bfb22010f32'
VCPKG_STATUS_SHA = 'c894d1796de7f727ca1a2fc680a9f5324a067da90a6a4b68f8c6233687914bec'
DEV022_ELF_SHA = '435ea5b41a664a0aedf166c96a140b13f265f923112533de9875e4e5040ada62'
UNITS = [('source', 'added10'), ('source', 'added40'), ('text', 'added10'), ('text', 'added40')]

env = os.environ.copy()
env.update(PATH=str(prior / 'toolchain/usr/bin') + ':' + env['PATH'],
           LD_LIBRARY_PATH=str(prior / 'toolchain/usr/lib/x86_64-linux-gnu'), LC_ALL='C.UTF-8')
os.environ.update(LD_LIBRARY_PATH=env['LD_LIBRARY_PATH'], LC_ALL=env['LC_ALL'])
progress = dict(phase='start', units=[])


def note(**kw):
    progress.update(kw, elapsed_s=time.monotonic() - started)
    write_json(results / 'progress.json', progress)


def run(argv, label, timeout=1800):
    print(json.dumps(dict(step=label, argv=list(map(str, argv)))), flush=True)
    with (results / (label + '.log')).open('w') as stream:
        proc = subprocess.run(list(map(str, argv)), env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    if proc.returncode:
        raise RuntimeError(label + ' failed; preserve evidence')


inputs = results / 'inputs'
inputs.mkdir()
for p in work.iterdir():
    if p.is_file():
        shutil.copyfile(p, inputs / p.name)
status = prior / 'build/vcpkg_installed/vcpkg/status'
if sha(status) != VCPKG_STATUS_SHA:
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
note(phase='build')
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
    raise ValueError('portable fixture archive hashes changed')
run([sys.executable, '-B', work / 'bounded_expand_smoke.py', tool, work / 'smoke'], 'smoke')
linux_smoke = json.loads((work / 'smoke/smoke-report.json').read_text(encoding='utf-8'))
windows_smoke = json.loads((work / 'windows-smoke-report.json').read_text(encoding='utf-8'))
shutil.copyfile(work / 'smoke/smoke-report.json', results / 'linux-smoke-report.json')
smoke_match = linux_smoke['archive_hashes'] == windows_smoke['archive_hashes']
remove_owned(work / 'smoke', work)
remove_owned(work / 'bounded-cross', work)
elf = sha(tool)
write_json(results / 'environment.json', dict(platform=platform.platform(), python=sys.version,
    cpu_count=os.cpu_count(), affinity=sorted(os.sched_getaffinity(0)),
    cpu_info=Path('/proc/cpuinfo').read_text(), memory_info=Path('/proc/meminfo').read_text(),
    tool_sha256=elf, same_elf_as_dev022=elf == DEV022_ELF_SHA, source_zip_sha256=sha(work / 'source.zip'),
    smoke_archives_match_windows=smoke_match))
if not smoke_match:
    raise ValueError('smoke archives differ between Windows and Linux')


def build_tree(cls):
    data = work / 'data' / cls
    data.mkdir(parents=True)
    manifest = work / f'{cls}.tsv'
    with zipfile.ZipFile(silesia_zip) as archive:
        for row in load(data, manifest, verify_content=False):
            rel = row['path']
            target = data / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            if rel.startswith('linux/'):
                src = linux_src / rel
            elif rel.startswith('enwik8/'):
                src = prior / 'data/wiki-text' / rel
            elif rel.startswith('silesia/'):
                info = archive.getinfo(rel)
                if info.file_size != row['size']:
                    raise ValueError('Silesia member size differs')
                with archive.open(info) as s, target.open('xb') as d:
                    shutil.copyfileobj(s, d)
                src = None
            else:
                raise ValueError('unexpected source namespace')
            if src is not None:
                if src.is_symlink() or not src.is_file():
                    raise ValueError('unexpected input source ' + rel)
                shutil.copyfile(src, target)
    rows = load(data, manifest)  # size, SHA256, no extra files
    write_json(results / f'data-{cls}.json', dict(files=len(rows), bytes=sum(r['size'] for r in rows),
        manifest_sha256=sha(manifest), scope='existing public dev extension; not a holdout'))
    return data, manifest


current = None
for cls, batch in UNITS:
    if time.monotonic() - started > BUDGET_S:
        note(phase='stopped: matrix budget reached before ' + f'{cls}-{batch}')
        break
    if current is None or current[0] != cls:
        if current is not None:
            remove_owned(current[1], work / 'data')
        note(phase='input ' + cls)
        data, manifest = build_tree(cls)
        current = (cls, data, manifest)
    unit = f'{cls}-{batch}'
    note(phase='unit ' + unit)
    t0 = time.monotonic()
    complete = run_unit(tool, current[1], current[2], cls, batch, work / unit, results / unit, task_root)
    remove_owned(work / unit, work)
    check = report.audit(results / unit)
    progress['units'].append(dict(unit=unit, wall_s=time.monotonic() - t0, audit_ok=check['ok'],
                                  problems=check['problems'], commands=complete['commands']))
    note(phase='audited ' + unit)
    if not check['ok']:
        raise AssertionError(unit + ' failed the independent audit; matrix stopped')
if current is not None:
    remove_owned(current[1], work / 'data')
done = [u['unit'] for u in progress['units']]
rc = subprocess.run([sys.executable, '-B', work / 'bounded_expand_report.py', results, results / 'analysis.json', *done],
                    env=env).returncode
write_json(results / 'task-complete.json', dict(units_completed=done, units_planned=[f'{c}-{b}' for c, b in UNITS],
    full_matrix=len(done) == len(UNITS), analysis_returncode=rc, tool_sha256=elf, same_elf_as_dev022=elf == DEV022_ELF_SHA,
    elapsed_s=time.monotonic() - started, sanitizer='not run: C++ identical to DEV-021/022 sources'))
note(phase='complete' if len(done) == len(UNITS) and rc == 0 else 'incomplete')
if rc:
    raise AssertionError('final analysis audit failed')
