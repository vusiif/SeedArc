# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed-toolchain India bootstrap for the first large Linux-source dev run."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

from append_history_r2 import sha, write_json

work=Path(os.environ['TASK_WORK']).resolve()
results=Path(os.environ['TASK_RESULTS']).resolve()
task_root=work.parent
prior=Path('/home/sc/long-tasks/20261123T123105Z-d5596074/work')
env=os.environ.copy()
env.update(PATH=str(prior/'toolchain/usr/bin')+':'+env['PATH'],
           LD_LIBRARY_PATH=str(prior/'toolchain/usr/lib/x86_64-linux-gnu'),LC_ALL='C.UTF-8')
compiler=prior/'toolchain/usr/bin/g++-14'
prefix=prior/'build/vcpkg_installed/x64-linux'
inputs=results/'inputs'
inputs.mkdir()
for path in work.iterdir():
    if path.is_file():
        shutil.copyfile(path,inputs/path.name)
write_json(results/'environment.json',dict(platform=platform.platform(),cpu_count=os.cpu_count(),
    affinity=sorted(os.sched_getaffinity(0)),memory_info=Path('/proc/meminfo').read_text().splitlines()[:5],
    cpu_info=next(s for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')),
    packages_sha256=sha(prior/'build/vcpkg_installed/vcpkg/status'),compiler=str(compiler),mode='exclusive'))
status=(prior/'build/vcpkg_installed/vcpkg/status').read_text()
for package,version in [('zstd','1.5.7'),('liblzma','5.8.4')]:
    if not any(('Package: '+package+'\n') in (p+'\n') and ('Version: '+version+'\n') in (p+'\n')
               for p in status.split('\n\n')):
        raise RuntimeError('fixed dependency changed')
(results/'dependency-status.txt').write_text(status)


def run(argv,name,timeout=1800):
    print(json.dumps(dict(step=name,argv=list(map(str,argv)))),flush=True)
    with (results/(name+'.log')).open('w') as out:
        proc=subprocess.run(list(map(str,argv)),env=env,stdout=out,stderr=subprocess.STDOUT,timeout=timeout)
    if proc.returncode:
        raise RuntimeError(name+' failed; inspect retained log')


source=work/'source'
source.mkdir()
with zipfile.ZipFile(work/'source.zip') as archive:
    for member in archive.infolist():
        path=PurePosixPath(member.filename)
        if path.is_absolute() or '..' in path.parts or '\\' in member.filename:
            raise ValueError('unsafe source archive')
        archive.extract(member,source)
build=work/'build'
run(['cmake','-S',source,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
     '-DCMAKE_CXX_COMPILER='+str(compiler),'-DCMAKE_PREFIX_PATH='+str(prefix),
     '-DSEEDARC_BUILD_RESEARCH_TOOLS=ON','-DBUILD_TESTING=ON'],'configure')
run(['cmake','--build',build,'--target','seedarc_append4_study','seedarc_path_check','--parallel','4'],'build')
tool=build/'research/tools/seedarc_append4_study'
checker=build/'research/tools/seedarc_path_check'
run(['ctest','--test-dir',build,'-R','^research.append4_study$','--output-on-failure'],'targeted-ctest')
shutil.copyfile(build/'CMakeCache.txt',results/'CMakeCache.txt')
run([sys.executable,'-B',work/'check_append_large_dev.py','--tool',tool,'--work',work/'scheduler-check'],'scheduler-check')
check=work/'scheduler-check'
shutil.copyfile(check/'acceptance.json',results/'scheduler-acceptance.json')
shutil.copyfile(check/'out-0/histories.jsonl',results/'scheduler-histories.jsonl')
from append_large_dev import remove_owned
remove_owned(check,work)

manifest=work/'dev-linux-512.tsv'
wanted={}
for line in manifest.read_text().splitlines():
    rel,size,digest,group,batch=line.split('\t')
    path=PurePosixPath(rel)
    if path.is_absolute() or '..' in path.parts or '\\' in rel or ':' in rel or not rel.startswith('linux/'):
        raise ValueError('unsafe data path')
    wanted['linux-6.12/'+rel.removeprefix('linux/')]=(rel,int(size),digest)
if sum(r[1] for r in wanted.values())>512*1024**2:
    raise ValueError('data exceeds validated scale')
archive_path=work/'linux-6.12.tar.xz'
with urllib.request.urlopen('https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz',timeout=120) as incoming, archive_path.open('xb') as outgoing:
    shutil.copyfileobj(incoming,outgoing,1024**2)
archive_sha=sha(archive_path)
if archive_sha!='b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb':
    raise ValueError('Linux archive SHA256 mismatch')
data=work/'data'
data.mkdir()
with tarfile.open(archive_path,'r|xz') as archive:
    for member in archive:
        if member.name not in wanted:
            continue
        rel,size,digest=wanted.pop(member.name)
        if not member.isfile() or member.size!=size:
            raise ValueError('unexpected selected archive member')
        dest=data/rel
        dest.parent.mkdir(parents=True,exist_ok=True)
        with archive.extractfile(member) as incoming,dest.open('xb') as outgoing:
            shutil.copyfileobj(incoming,outgoing,1024**2)
        if sha(dest)!=digest:
            raise ValueError('data member SHA256 mismatch')
if wanted:
    raise ValueError('missing selected data')
archive_path.unlink()
proc=subprocess.run([str(checker),'--manifest'],env=env,input=''.join(line.split('\t')[0]+'\n' for line in manifest.read_text().splitlines()),
                    text=True,capture_output=True,check=True)
if not proc.stdout.startswith('ok\t'):
    raise ValueError('Linux production manifest check failed')
write_json(results/'inputs-verified.json',dict(archive_sha256=archive_sha,manifest_sha256=sha(manifest),
    files=len(manifest.read_text().splitlines()),path_check=proc.stdout.strip(),holdout_contents_opened=False))
run([sys.executable,'-B',work/'append_large_dev.py','--tool',tool,'--input',data,'--manifest',manifest,
     '--work',work/'histories','--output',results/'dev','--task-root',task_root,
     '--budget-multiple','4','--prng-seed','3'],'large-dev',timeout=3600)
write_json(results/'task-complete.json',dict(status='large dev exploratory run complete',
    tool_sha256=sha(tool),source_zip_sha256=sha(work/'source.zip'),scope='single-run cost observation'))
