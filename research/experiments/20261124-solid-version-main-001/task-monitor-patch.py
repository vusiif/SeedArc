import importlib.util
from pathlib import Path
import subprocess
import hashlib
import base64
import json
import re
import sys

sys.path.insert(0,str(Path('research/scripts').resolve()))
from append_monitor_guard import patch

task=sys.argv[1]
if not re.fullmatch(r'[0-9]{8}T[0-9]{6}Z-[a-f0-9]{8}',task):
    raise ValueError('invalid task ID')
skill=Path.home()/'.codex/skills/in-long-task/scripts/in_task.py'
spec=importlib.util.spec_from_file_location('india_client',skill)
client=importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
original=skill.with_name('remote_job.py').read_text()
patched=patch(original)
request=dict(task=task,before=hashlib.sha256(original.encode()).hexdigest(),
             after=hashlib.sha256(patched.encode()).hexdigest(),data=base64.b64encode(patched.encode()).decode())
source='''import pathlib,sys,json,hashlib,base64
root=pathlib.Path('/home/sc/long-tasks')/request['task']
runner=root/'runner.py'
if root.is_symlink() or runner.is_symlink() or (root/'submitted.json').exists():
 raise RuntimeError('only a fresh prepared task runner may be patched')
manifest=json.loads((root/'manifest.json').read_text())
if manifest['mode']!='exclusive' or manifest['disk_limit_bytes']!=2*1024**3:
 raise RuntimeError('unexpected task limits')
before=runner.read_bytes()
if hashlib.sha256(before).hexdigest()!=request['before']:
 raise RuntimeError('runner differs from audited source')
data=base64.b64decode(request['data'],validate=True)
if hashlib.sha256(data).hexdigest()!=request['after']:
 raise RuntimeError('patched runner hash mismatch')
compile(data,str(runner),'exec')
runner.write_bytes(data)
record={'task_id':request['task'],'original_sha256':request['before'],'patched_sha256':request['after'],
 'disk_limit_bytes':manifest['disk_limit_bytes'],'scope':'task runner copy only; installed skill unchanged'}
(root/'results/monitor-patch.json').write_text(json.dumps(record,indent=2))
print(json.dumps(record))
'''
launcher="python3 -c 'import json,sys; p=json.load(sys.stdin); exec(compile(p[\"source\"],\"task_monitor_patch\",\"exec\"),{\"request\":p[\"request\"]})'"
payload=json.dumps(dict(source=source,request=request)).encode()
r=subprocess.run(client.connection()+['-T','sc@'+client.HOST,launcher],input=payload,capture_output=True,timeout=60,check=True)
record=json.loads(r.stdout)
Path('research/experiments/20261124-solid-version-main-001/monitor-patch.json').write_text(json.dumps(record,indent=2))
print(json.dumps(record))
