import importlib.util,json,subprocess
from pathlib import Path
skill=Path.home()/'.codex/skills/in-long-task/scripts/in_task.py'
spec=importlib.util.spec_from_file_location('client',skill)
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
source='''import json,pathlib
r=pathlib.Path('/home/sc/long-tasks/20261124T082849Z-65827d9a')
p=r/'logs/combined.log'
print(json.dumps(dict(tail=p.read_text(errors='replace').splitlines()[-4:] if p.exists() else [],
results=[str(p.relative_to(r/'results')) for p in (r/'results').glob('*.log')],
progress=[dict(label=x['label'],returncode=x['returncode'],seconds=x['process_wall_ms']/1000,
 bytes=x.get('result',{}).get('container_bytes'),plan=x.get('result',{}).get('best_plan'),baseline_bytes=x.get('result',{}).get('baseline_bytes'))
 for x in list(map(json.loads,(r/'results/matrix/raw.jsonl').read_text().splitlines()))[-4:]] if (r/'results/matrix/raw.jsonl').exists() else [],
archive_summary={x['label']:dict(bytes=x['bytes'],call_seconds=x['result']['call_wall_ms']/1000) for x in json.loads((r/'results/matrix/archives.json').read_text())} if (r/'results/matrix/archives.json').exists() else {},
checks={p.name:p.read_text(errors='replace').splitlines()[-4:] for p in (r/'results').glob('*ctest.log')},
version={k:json.loads((r/'results/version-data/version-delta.json').read_text())[k] for k in ['changed_files','unchanged_files','additions','deletions','renames']} if (r/'results/version-data/version-delta.json').exists() else None,
finished=(r/'result.json').read_text() if (r/'result.json').exists() else None)))
'''
launcher="python3 -c 'import json,sys; exec(compile(json.load(sys.stdin),\"observe\",\"exec\"))'"
r=subprocess.run(c.connection()+['-T','sc@'+c.HOST,launcher],input=json.dumps(source).encode(),capture_output=True,timeout=50,check=True)
print(r.stdout.decode())
