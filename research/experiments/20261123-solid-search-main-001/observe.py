import importlib.util,json,subprocess
from pathlib import Path
skill=Path.home()/'.codex/skills/in-long-task/scripts/in_task.py'
spec=importlib.util.spec_from_file_location('client',skill)
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
source='''import json,pathlib
r=pathlib.Path('/home/sc/long-tasks/20261124T075308Z-0ab63a9b')
p=r/'logs/combined.log'
print(json.dumps(dict(tail=p.read_text(errors='replace').splitlines()[-12:] if p.exists() else [],
results=[str(p.relative_to(r/'results')) for p in (r/'results').glob('*.log')],
progress=[dict(label=x['label'],returncode=x['returncode'],seconds=x['process_wall_ms']/1000,
 bytes=x.get('result',{}).get('container_bytes'),plan=x.get('result',{}).get('best_plan'))
 for x in map(json.loads,(r/'results/search/raw.jsonl').read_text().splitlines())] if (r/'results/search/raw.jsonl').exists() else [],
checks={p.name:p.read_text(errors='replace').splitlines()[-4:] for p in (r/'results').glob('*ctest.log')},
finished=(r/'result.json').read_text() if (r/'result.json').exists() else None)))
'''
launcher="python3 -c 'import json,sys; exec(compile(json.load(sys.stdin),\"observe\",\"exec\"))'"
r=subprocess.run(c.connection()+['-T','sc@'+c.HOST,launcher],input=json.dumps(source).encode(),capture_output=True,timeout=50,check=True)
print(r.stdout.decode())
