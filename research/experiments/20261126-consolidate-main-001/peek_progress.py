# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only: print a DEV-023 task's results/progress.json and the last log lines."""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

task = sys.argv[1]
if not re.fullmatch(r'[0-9]{8}T[0-9]{6}Z-[a-f0-9]{8}', task):
    raise ValueError('invalid task ID')
skill = Path.home() / '.claude/skills/in-long-task/scripts/in_task.py'
spec = importlib.util.spec_from_file_location('india_client', skill)
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
source = '''import pathlib,json
root=pathlib.Path('/home/sc/long-tasks')/task
p=root/'results/progress.json'
log=root/'logs/combined.log'
out={'progress':json.loads(p.read_text()) if p.is_file() else None}
if log.is_file():
    out['log_tail']=log.read_text(errors='replace').splitlines()[-int(lines):]
print(json.dumps(out))
'''
launcher = "python3 -c 'import json,sys; p=json.load(sys.stdin); exec(p[\"source\"],{\"task\":p[\"task\"],\"lines\":p[\"lines\"]})'"
payload = json.dumps(dict(source=source, task=task, lines=int(sys.argv[2]) if len(sys.argv) > 2 else 8)).encode()
r = subprocess.run(client.connection() + ['-T', 'sc@' + client.HOST, launcher], input=payload,
                   capture_output=True, timeout=60, check=True)
print(json.dumps(json.loads(r.stdout), indent=1, ensure_ascii=False))
