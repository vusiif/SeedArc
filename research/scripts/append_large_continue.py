# SPDX-License-Identifier: GPL-3.0-or-later
"""Continue only unmeasured branches after the diagnosed monitor failure."""
import json
import os
from pathlib import Path
import shutil

from append_large_dev import run, rows
from append_history_r2 import sha, write_json

work=Path(os.environ['TASK_WORK']).resolve()
results=Path(os.environ['TASK_RESULTS']).resolve()
prior=Path('/home/sc/long-tasks/20261124T051435Z-cef36683/work')
manifest=work/'dev-linux-512.tsv'
source_tool=prior/'build/research/tools/seedarc_append4_study'
if sha(source_tool)!='b9afd557babd5653881af6451bb37474789e6448c0a463a615bf4c777a3780a9':
    raise ValueError('continuation binary differs')
if sha(work/'append_large_dev.py')!='f11cd766169ae59d9efa49b9387dd15d8c85db321205272849fb42671b57790b':
    raise ValueError('continuation runner differs')
if sha(manifest)!='8ea19a4df1e4bb13a9c82595de6e54abad5c8a9e78ec086e7e29c32566f1da91':
    raise ValueError('continuation manifest differs')
selected=rows(prior/'data',manifest)
data=work/'data'
data.mkdir()
for row in selected:
    dest=data/row['path']
    dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(prior/'data'/row['path'],dest)
tool=work/'seedarc_append4_study'
shutil.copy2(source_tool,tool)
inputs=results/'inputs'
inputs.mkdir()
for name in ['append_large_continue.py','append_large_dev.py','append_history_r2.py','dev-linux-512.tsv']:
    shutil.copyfile(work/name,inputs/name)
write_json(results/'continuation.json',dict(previous_task='20261124T051435Z-cef36683',
    reason='task disk monitor FileNotFoundError during verified-file reclamation',
    same_tool_sha256=sha(tool),same_runner_sha256=sha(work/'append_large_dev.py'),
    same_manifest_sha256=sha(manifest),source_copy='verified source copied into this task; not shared disk-budget bypass',
    skipped_completed_branches=['A','B-new','C','C-coord','D-nearest']))
summary=run(tool,data,manifest,work/'histories',results/'dev',work.parent,4,3,
            branches=['S-segment','S-regroup','R-seed','R-coord'])
write_json(results/'task-complete.json',dict(status='remaining branches complete',summary=summary))
print(json.dumps(summary))
