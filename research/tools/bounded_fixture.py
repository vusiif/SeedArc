# SPDX-License-Identifier: GPL-3.0-or-later
"""Write identical UTF-8/LF byte fixtures on Windows and Linux."""
from pathlib import Path
import sys

root=Path(sys.argv[1])
root.mkdir(exist_ok=False)
for i in range(16):
    content=(f'data-{i}: alpha beta 中文\n'*(i*20+60)).encode('utf-8')[:1700]
    rel=f'源-{i}/文件-{i}.txt'
    for directory in ['first' if i<10 else 'added','all']:
        p=root/directory/rel
        p.parent.mkdir(parents=True,exist_ok=True)
        p.write_bytes(content)
for directory in ['first','all']:
    (root/directory/'exact').write_bytes(b'X'*4096)
    (root/directory/'empty').write_bytes(b'')
