import importlib.util
from pathlib import Path
import subprocess
import json

spec=importlib.util.spec_from_file_location('india_client',Path.home()/'.codex/skills/in-long-task/scripts/in_task.py')
client=importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
source='''import pathlib,json,time,hashlib
root=pathlib.Path('/home/sc/long-tasks/20261124T065628Z-509e5ae7')
modes=['solid','seed-solid','explicit-solid']
seen={}
deadline=time.monotonic()+1200
while len(seen)<3 and time.monotonic()<deadline:
 raw=root/'results/zero/raw.jsonl'
 if raw.exists():
  text=raw.read_text(); rows=[]
  for line in text.splitlines():
   try: rows.append(json.loads(line))
   except json.JSONDecodeError:
    if text.endswith('\\n'): raise
  verified={r['label'] for r in rows if r['returncode']==0}
  for mode in modes:
   if mode in seen or mode+'-regroup-verify' not in verified: continue
   p=root/'work/zero-work'/(mode+'-regroup.sadx')
   try:
    before=p.stat()
    with p.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
    after=p.stat()
   except FileNotFoundError: continue
   if (before.st_ino,before.st_size,before.st_mtime_ns)!=(after.st_ino,after.st_size,after.st_mtime_ns):
    raise RuntimeError('archive changed while hashing')
   seen[mode]={'bytes':after.st_size,'sha256':digest,'verified_label':mode+'-regroup-verify'}
 if (root/'result.json').exists() and len(seen)<3: break
 if len(seen)<3: time.sleep(1)
print(json.dumps({'scope':'independent read-only hashes after verify and before reclamation','archives':seen,'complete':len(seen)==3}))
if len(seen)!=3: raise SystemExit(1)
'''
r=subprocess.run(client.connection()+['-T','sc@'+client.HOST,'python3 -'],input=source.encode(),capture_output=True,timeout=1230)
if r.stdout:
    received=json.loads(r.stdout)
    Path('research/experiments/20261123-solid-seed-main-001/regroup-hashes.json').write_text(json.dumps(received,indent=2))
    print(json.dumps(dict(archives=len(received['archives']),complete=received['complete'])))
if r.returncode:
    print(r.stderr.decode(errors='replace')[-1000:])
    raise SystemExit(r.returncode)
