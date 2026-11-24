import importlib.util,json,subprocess,base64,hashlib
from pathlib import Path
skill=Path.home()/'.codex/skills/in-long-task/scripts/in_task.py'
spec=importlib.util.spec_from_file_location('client',skill);c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
files={n:base64.b64encode((Path('research/tools')/n).read_bytes()).decode() for n in ['check_bounded_solid.cmake','bounded_fixture.py']}
remote_code=r"""import pathlib,json,base64,hashlib,subprocess,os
root=pathlib.Path('/home/sc/long-tasks/20261124T092901Z-14334bcf')
if json.loads((root/'result.json').read_text())['exit_code']!=0: raise RuntimeError('original task not complete')
work=root/'work/post-cross'
if (root/'results/post-cross-normalization.json').exists() or (work/'fixture').exists(): raise RuntimeError('post-check already ran')
work.mkdir(exist_ok=True)
hashes={}
for name,encoded in request.items():
    if name not in ['check_bounded_solid.cmake','bounded_fixture.py']: raise ValueError('unexpected input')
    data=base64.b64decode(encoded,validate=True)
    (work/name).write_bytes(data);hashes[name]=hashlib.sha256(data).hexdigest()
env=os.environ.copy();env['PATH']='/home/sc/long-tasks/20261123T123105Z-d5596074/work/toolchain/usr/bin:'+env.get('PATH','');env['LD_LIBRARY_PATH']='/home/sc/long-tasks/20261123T123105Z-d5596074/work/toolchain/usr/lib/x86_64-linux-gnu'
tool=root/'work/release/research/tools/seedarc_append4_study'
expected=json.loads((root/'results/task-complete.json').read_text())['binary_sha256']
if hashlib.sha256(tool.read_bytes()).hexdigest()!=expected: raise ValueError('binary changed')
argv=['/home/sc/long-tasks/20261123T123105Z-d5596074/work/toolchain/usr/bin/cmake','-DTOOL='+str(tool),'-DWORK='+str(work/'fixture'),'-DREPORT='+str(root/'results/linux-normalized-cross-hashes.json'),'-P',str(work/'check_bounded_solid.cmake')]
with (root/'results/post-cross-normalization.log').open('w') as stream:
    p=subprocess.run(argv,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=600)
record=dict(task_id=root.name,exit_code=p.returncode,binary_sha256=expected,test_source_hashes=hashes,scope='post-run fixture LF normalization only; benchmark/C++ unchanged')
(root/'results/post-cross-normalization.json').write_text(json.dumps(record,indent=2))
if p.returncode: raise RuntimeError('post-cross test failed')
print(json.dumps(record))
"""
launcher="python3 -c 'import json,sys; p=json.load(sys.stdin); exec(compile(p[\"source\"],\"post_cross\",\"exec\"),{\"request\":p[\"files\"]})'"
p=subprocess.run(c.connection()+['-T','sc@'+c.HOST,launcher],input=json.dumps(dict(source=remote_code,files=files)).encode(),capture_output=True,timeout=90)
if p.returncode: raise RuntimeError(p.stderr.decode(errors='replace'))
record=json.loads(p.stdout)
Path('research/experiments/20261124-bounded-solid-main-001/post-cross-normalization-local.json').write_text(json.dumps(record,indent=2))
print(json.dumps(record))
