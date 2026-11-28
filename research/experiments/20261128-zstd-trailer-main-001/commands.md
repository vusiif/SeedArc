# DEV-044 命令与临时脚本

在本机仓库根目录运行。`$S` 是会话临时目录，`$TOOL` 是 build/dictstudy-check 的 Release 版 seedarc_append4_study.exe（SHA256 前缀 4ed1c0de…）。

```
python -I -B $S/trailermeasure.py $TOOL research/experiments/20261128-zstd-trailer-main-001/local/raw.jsonl $S/tm-w append-r1-dev=research/local/append-r1/files/dev cargo=research/local/dup-local-dev-r1/files/cargo rustup=research/local/dup-local-dev-r1/files/rustup all=research/local/dup-local-dev-r1/files npm=research/local/dup-local-dev-r1/files/npm
# 打开索引的代价：先 --trailer raw recipe-pack seed solid/zstd-3/g1-r0/frame-32MiB 打包 dup-local-dev-r1/files，再 trailer-recode 成 zstd；
# 然后对两个包交替各运行 5 次：extract ARCHIVE cargo/lazy_static-1.5.1/Cargo.lock OUTDIR NAME，读取 index_ms。
```

## trailermeasure.py

```python
"""DEV-044: ADR-0010 compressed trailer on the DEV-040 trees (local).
python -I -B trailermeasure.py TOOL OUT_JSONL WORK TREE=DIR ...
Part 1: solid/zstd-3/g1-r0/frame-32MiB (as DEV-041) packed with --trailer raw, then trailer-recode
to zstd: the exact saving of the same records. Part 2: the DEV-043 recipe
solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup packed by default (zstd trailer) and verified.
"""
import hashlib, json, subprocess, sys, time
from pathlib import Path

tool, out, work = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
work.mkdir(parents=True, exist_ok=False)


def sha(p):
    with open(p, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run(*args):
    t = time.perf_counter()
    r = subprocess.run([tool, *args], capture_output=True, check=True)
    return r.stdout, round(time.perf_counter() - t, 3)


def describe(arc):
    return json.loads(run('describe', str(arc))[0])['trailer']


with open(out, 'a', encoding='utf-8') as res:
    res.write(json.dumps(dict(tool_sha256=sha(tool))) + '\n')
    for spec in sys.argv[4:]:
        name, root = spec.split('=', 1)
        raw, z = work / 'raw.sadx', work / 'z.sadx'
        run('--trailer', 'raw', 'recipe-pack', 'seed', 'solid/zstd-3/g1-r0/frame-32MiB', root, str(raw))
        _, recode_s = run('trailer-recode', str(raw), str(z), 'zstd')
        _, verify_s = run('verify', str(z), root)
        row = dict(tree=name, part='recode', recipe='solid/zstd-3/g1-r0/frame-32MiB', raw_archive=raw.stat().st_size,
                   zstd_archive=z.stat().st_size, raw_trailer=describe(raw), zstd_trailer=describe(z),
                   recode_wall_s=recode_s, verify_wall_s=verify_s)
        res.write(json.dumps(row) + '\n'); res.flush(); print(json.dumps(row), flush=True)
        raw.unlink(); z.unlink()
        recipe = 'solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup'
        info, pack_s = run('--jobs', '1', 'recipe-pack', 'seed', recipe, root, str(z))
        _, verify_s = run('verify', str(z), root)
        row = dict(tree=name, part='final', recipe=recipe, archive_bytes=z.stat().st_size,
                   parts=json.loads(info)['parts'], trailer=describe(z), pack_call_s=pack_s, verify_wall_s=verify_s)
        res.write(json.dumps(row) + '\n'); res.flush(); print(json.dumps(row), flush=True)
        z.unlink()
work.rmdir()
```
