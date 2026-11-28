# DEV-043 命令与临时脚本

在本机仓库根目录运行；`$S` 是会话临时目录，`$TOOL` 是 build/dictstudy-check 的 Release 版 seedarc_append4_study.exe（SHA256 1a2a5ebb…）。

```
cmake --build build/dictstudy-check --config Release --parallel
ctest --test-dir build/dictstudy-check -C Release -j 4
python -I -B $S/dictmeasure.py . $TOOL research/experiments/20261128-lzma2-dict-main-001/local/raw.jsonl $S/dm-w append-r1-dev=research/local/append-r1/files/dev cargo=research/local/dup-local-dev-r1/files/cargo rustup=research/local/dup-local-dev-r1/files/rustup all=research/local/dup-local-dev-r1/files
```

## dictmeasure.py

```python
"""DEV-043: LZMA2 dictionary sizes (ADR-0011) on the DEV-040 trees, local, --jobs 1.
python -I -B dictmeasure.py REPO TOOL OUT_JSONL WORK TREE=DIR ...
"""
import importlib.util, json, sys
from pathlib import Path

repo, tool, out, work = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
spec = importlib.util.spec_from_file_location('eb', repo / 'research/scripts/external_baseline.py')
eb = importlib.util.module_from_spec(spec); spec.loader.exec_module(eb)
work.mkdir(parents=True, exist_ok=False)
RECIPES = ['solid/lzma2-9e/g1-r0/frame-whole/dedup', 'solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup',
           'solid/lzma2-9e-dict1024MiB/g1-r0/frame-whole/dedup']
with open(out, 'a', encoding='utf-8') as res:
    res.write(json.dumps(dict(tool_sha256=eb.sha(tool))) + '\n')
    for spec_ in sys.argv[5:]:
        name, root = spec_.split('=', 1)
        for recipe in RECIPES:
            arc = work / 'a.sadx'
            with open(work / 'pack.json', 'wb') as f:
                m, _ = eb.run([tool, '--jobs', '1', 'recipe-pack', 'seed', recipe, root, str(arc)], stdout=f)
            info = json.loads((work / 'pack.json').read_text(encoding='utf-8'))
            v, _ = eb.run([tool, 'verify', str(arc), root])
            row = dict(tree=name, recipe=recipe, archive_bytes=arc.stat().st_size, parts=info['parts'],
                       pack_wall_s=round(info['pack_wall_ms'] / 1000, 3), verify_wall_s=v['wall_s'],
                       verify_peak_bytes=v['peak_bytes'], **m)
            res.write(json.dumps(row) + '\n'); res.flush(); print(json.dumps(row), flush=True)
            arc.unlink()
(work / 'pack.json').unlink()
work.rmdir()
```
