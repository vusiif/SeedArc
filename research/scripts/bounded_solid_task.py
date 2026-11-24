# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed India build, sanitizer checks and bounded solid lifecycle dev."""
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import zipfile

from append_history_r2 import sha, write_json
from append_large_dev import rows, remove_owned

work=Path(os.environ['TASK_WORK']).resolve()
results=Path(os.environ['TASK_RESULTS']).resolve()
prior=Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')
old=Path('/home/sc/long-tasks/20261124T051435Z-cef36683/work')
env=os.environ.copy()
env.update(PATH=str(prior/'toolchain/usr/bin')+':'+env['PATH'],
           LD_LIBRARY_PATH=str(prior/'toolchain/usr/lib/x86_64-linux-gnu'),LC_ALL='C.UTF-8')
os.environ.update(LD_LIBRARY_PATH=env['LD_LIBRARY_PATH'],LC_ALL=env['LC_ALL'])
inputs=results/'inputs'
inputs.mkdir()
for p in work.iterdir():
    if p.is_file():
        shutil.copyfile(p,inputs/p.name)
status=prior/'build/vcpkg_installed/vcpkg/status'
if sha(status)!='c894d1796de7f727ca1a2fc680a9f5324a067da90a6a4b68f8c6233687914bec':
    raise ValueError('fixed dependency status changed')
shutil.copyfile(status,results/'dependency-status.txt')
source=work/'source'
source.mkdir()
with zipfile.ZipFile(work/'source.zip') as z:
    for entry in z.infolist():
        p=PurePosixPath(entry.filename)
        if p.is_absolute() or '..' in p.parts or '\\' in entry.filename:
            raise ValueError('unsafe source entry')
        z.extract(entry,source)


def run(argv,label,timeout=1800):
    print(json.dumps(dict(step=label,argv=list(map(str,argv)))),flush=True)
    with (results/(label+'.log')).open('w') as stream:
        proc=subprocess.run(list(map(str,argv)),env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=timeout)
    if proc.returncode:
        raise RuntimeError(label+' failed; preserve evidence')


base=['-G','Ninja','-DCMAKE_CXX_COMPILER='+str(prior/'toolchain/usr/bin/g++-14'),
      '-DCMAKE_PREFIX_PATH='+str(prior/'build/vcpkg_installed/x64-linux'),
      '-DSEEDARC_BUILD_RESEARCH_TOOLS=ON','-DBUILD_TESTING=ON']
release=work/'release'
run(['cmake','-S',source,'-B',release,'-DCMAKE_BUILD_TYPE=Release',*base],'release-configure')
run(['cmake','--build',release,'--parallel','4'],'release-build')
run(['ctest','--test-dir',release,'--output-on-failure','--parallel','1'],'release-ctest')
shutil.copyfile(release/'CMakeCache.txt',results/'release-cache.txt')
tool=release/'research/tools/seedarc_append4_study'
legacy=old/'build/research/tools/seedarc_append4_study'
if sha(legacy)!='b9afd557babd5653881af6451bb37474789e6448c0a463a615bf4c777a3780a9':
    raise ValueError('legacy comparison binary changed')
run(['cmake','-DTOOL='+str(tool),'-DBEFORE_TOOL='+str(legacy),'-DWORK='+str(work/'compat'),
     '-P',source/'research/tools/check_solid_seed.cmake'],'legacy-compatibility')
asan=work/'asan'
run(['cmake','-S',source,'-B',asan,'-DCMAKE_BUILD_TYPE=Debug',
     '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer',
     '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined',*base],'asan-configure')
run(['cmake','--build',asan,'--target','seedarc_append4_study','--parallel','2'],'asan-build')
run(['ctest','--test-dir',asan,'--output-on-failure','--parallel','1','-R','^research.(append4_study|solid_seed|solid_search|bounded_solid)$'],'asan-ctest')
shutil.copyfile(asan/'CMakeCache.txt',results/'asan-cache.txt')
shutil.copyfile(asan/'Testing/Temporary/LastTest.log',results/'asan-last-test.log')
remove_owned(asan,work)

run(['cmake','-DTOOL='+str(tool),'-DWORK='+str(work/'bounded-cross'),
     '-DREPORT='+str(results/'linux-cross-hashes.json'),'-P',
     source/'research/tools/check_bounded_solid.cmake'],'bounded-cross')
write_json(results/'environment.json',dict(platform=__import__('platform').platform(),
    cpu_info=Path('/proc/cpuinfo').read_text(),memory_info=Path('/proc/meminfo').read_text()))
manifest=work/'v0.tsv'
data=work/'data'
data.mkdir()
with zipfile.ZipFile(work/'silesia-dev.zip') as archive:
    expected={line.split('\t')[0] for line in manifest.read_text().splitlines() if line.startswith('silesia/')}
    if set(archive.namelist())!=expected: raise ValueError('unexpected Silesia members')
    for line in manifest.read_text().splitlines():
        rel,size,digest,group,batch=line.split('\t')
        p=PurePosixPath(rel)
        if p.is_absolute() or '..' in p.parts or p.as_posix()!=rel or '\\' in rel or ':' in rel:
            raise ValueError('unsafe manifest path')
        target=data/rel
        target.parent.mkdir(parents=True,exist_ok=True)
        if rel.startswith('linux/'):
            source_file=old/'data'/rel
            if source_file.is_symlink() or not source_file.is_file(): raise ValueError('unexpected Linux source')
            shutil.copyfile(source_file,target)
        elif rel.startswith('enwik8/'):
            source_file=prior/'data/wiki-text'/rel
            if source_file.is_symlink() or not source_file.is_file(): raise ValueError('unexpected wiki source')
            shutil.copyfile(source_file,target)
        elif rel in expected:
            info=archive.getinfo(rel)
            if info.file_size!=int(size) or info.file_size>40*1024**2: raise ValueError('Silesia bound differs')
            with archive.open(info) as src,target.open('xb') as dest: shutil.copyfileobj(src,dest)
        else: raise ValueError('unexpected source namespace')
        if target.stat().st_size!=int(size) or sha(target)!=digest: raise ValueError('input hash mismatch')
selected=rows(data,manifest)
write_json(results/'data-provenance.json',dict(files=len(selected),bytes=sum(r['size'] for r in selected),
    manifest_sha256=sha(manifest),scope='existing mixed public dev, no holdout'))
from bounded_solid_dev import run as bounded_run
bounded_run(tool,data,manifest,work/'histories',results/'bounded',work.parent)
write_json(results/'task-complete.json',dict(status='bounded solid dev matrix complete',
    binary_sha256=sha(tool),source_zip_sha256=sha(work/'source.zip')))
