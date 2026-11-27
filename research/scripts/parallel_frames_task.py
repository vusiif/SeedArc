# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-035 India task: fixed-toolchain Release build + full CTest, writer regression check, then
parallel frame encoding (`--jobs N`) on both DEV-023 trees.

Recipes LZMA2-9e, LZMA2-6e and Zstd-19 at the 32 MiB frame step, `--jobs` 1 / 2 / 4, two rounds
(second round in reverse order). Exclusive task, commands unpinned so every worker count can use
all CPUs; one command at a time. Every archive of a recipe must have the same SHA256 for every
worker count and round, and its size must equal the reported container bytes; the jobs 4 archive
of the first round is verified byte for byte against the tree.
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

from bounded_expand_dev import load, remove_owned, sha, write_json
from codec_curve_dev import Runner

if sys.argv[1:]:
    raise SystemExit('usage: parallel_frames_task.py')
work = Path(os.environ['TASK_WORK']).resolve()
results = Path(os.environ['TASK_RESULTS']).resolve()
task_root = work.parent
started = time.monotonic()
prior = Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')
linux_src = Path('/home/sc/long-tasks/20261124T051435Z-cef36683/work/data')
silesia_zip = Path('/home/sc/long-tasks/20261124T102611Z-2912f5b3/results/inputs/silesia-dev.zip')
SILESIA_ZIP_SHA = '7f7efa49d4d71a6af5fc8bb295ea6696ea05a908fcc80fa724168bfb22010f32'
VCPKG_STATUS_SHA = 'c894d1796de7f727ca1a2fc680a9f5324a067da90a6a4b68f8c6233687914bec'
CLASSES = ['source', 'text']

env = os.environ.copy()
env.update(PATH=str(prior / 'toolchain/usr/bin') + ':' + env['PATH'],
           LD_LIBRARY_PATH=str(prior / 'toolchain/usr/lib/x86_64-linux-gnu'), LC_ALL='C.UTF-8')
os.environ.update(LD_LIBRARY_PATH=env['LD_LIBRARY_PATH'], LC_ALL=env['LC_ALL'])
progress = dict(phase='start')


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
    raise ValueError('writer output changed: portable fixture archive hashes differ')
remove_owned(work / 'bounded-cross', work)
write_json(results / 'environment.json', dict(platform=platform.platform(), python=sys.version,
    cpu_count=os.cpu_count(), affinity=sorted(os.sched_getaffinity(0)),
    cpu_info=Path('/proc/cpuinfo').read_text(), memory_info=Path('/proc/meminfo').read_text(),
    tool_sha256=sha(tool), source_zip_sha256=sha(work / 'source.zip')))


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
    load(data, manifest)  # size, SHA256, no extra files
    return data, manifest


RECIPES = ['solid/lzma2-9e/g1-r0/frame-32MiB', 'solid/lzma2-6e/g1-r0/frame-32MiB', 'solid/zstd-19/g1-r0/frame-32MiB']
JOBS = [1, 2, 4]
ROUNDS = 2


def jobs_run():
    runner = Runner(tool, results / 'jobs', task_root, pin_cpu=False, timeout=7200)
    hashes = {}
    for cls in CLASSES:
        note(phase='jobs ' + cls)
        data, manifest = build_tree(cls)
        points = [(r, j) for r in RECIPES for j in JOBS]
        for rnd in range(ROUNDS):
            for recipe, jobs in (points if rnd % 2 == 0 else list(reversed(points))):
                note(phase=f'{cls} round {rnd} {recipe} jobs {jobs}')
                out = work / 'pack.sadx'
                packed = runner(['--jobs', jobs, 'recipe-pack', 'seed', recipe, data, out],
                                f'{cls}-r{rnd}-{recipe.split("/")[1]}-j{jobs}',
                                dict(cls=cls, round=rnd, recipe=recipe, jobs=jobs), warm=[data])
                digest = sha(out)
                if packed['recipe'] != recipe or out.stat().st_size != packed['container_bytes']:
                    raise AssertionError(f'{cls} {recipe} jobs {jobs}: archive does not match its report')
                if hashes.setdefault((cls, recipe), digest) != digest:
                    raise AssertionError(f'{cls} {recipe} jobs {jobs}: bytes depend on the worker count')
                if rnd == 0 and jobs == JOBS[-1]:
                    runner(['verify', out, data], f'{cls}-verify-{recipe.split("/")[1]}',
                           dict(cls=cls, recipe=recipe, mode='verify'))
                out.unlink()
        remove_owned(data, work / 'data')
    write_json(results / 'archive-hashes.json', {f'{c} {r}': h for (c, r), h in hashes.items()})
    write_json(results / 'task-complete.json', dict(commands=len(runner.records), tool_sha256=sha(tool),
        elapsed_s=time.monotonic() - started))


jobs_run()
note(phase='complete')
