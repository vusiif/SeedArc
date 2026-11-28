# DEV-042 命令与临时脚本

在本机仓库根目录运行；`$S` 是会话临时目录，不入库。

```
python -I $S/z7ablate.py research/experiments/20261128-7zip-version-main-001/local/ablate.jsonl $S/z7w rustup=research/local/dup-local-dev-r1/files/rustup cargo=research/local/dup-local-dev-r1/files/cargo
curl -fsSL -o 7zip_23.01+dfsg-11_amd64.deb https://archive.ubuntu.com/ubuntu/pool/universe/7/7zip/7zip_23.01%2Bdfsg-11_amd64.deb   # SHA256 0f79450d81e64326a862a8645ae1613890e486115101e51e41ca07d929388d87
dpkg-deb -x 7zip_23.01+dfsg-11_amd64.deb root   # (WSL) root/usr/lib/7zip/7z SHA256 b2a12fa672bc38fc33af43a99fa499533253236feb30e4f755a73fb64f01861d
python -I $S/z7round2.py . research/experiments/20261128-7zip-version-main-001/local/round2.jsonl $S/z7w2 rustup=research/local/dup-local-dev-r1/files/rustup cargo=research/local/dup-local-dev-r1/files/cargo
```

## z7ablate.py

```python
"""DEV-042 7-Zip version gap: ablation of 7-Zip 26.03 (Windows) and 26.00 (WSL) on the DEV-040 trees.
python -I z7ablate.py OUT_JSONL WORK TREE_NAME=WINDIR ...
"""
import json, subprocess, sys, time, re
from pathlib import Path

Z7 = r'<7za.exe>'
out, work = Path(sys.argv[1]), Path(sys.argv[2])
work.mkdir(parents=True, exist_ok=False)
VARIANTS = [('win26.03 -mx9', []), ('win26.03 -mx9 -mf=off', ['-mf=off']),
            ('win26.03 -mx9 -mqs=on', ['-mqs=on']), ('win26.03 -mx9 -mqs=off', ['-mqs=off'])]


def wslpath(p):
    p = str(Path(p).resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]


def methods(listing):
    folders = {}
    for line in listing.splitlines():
        m = re.match(r'Method = (.*)', line)
        if m:
            folders[m.group(1)] = folders.get(m.group(1), 0) + 1
    return folders


with open(out, 'a', encoding='utf-8') as res:
    for spec in sys.argv[3:]:
        name, root = spec.split('=', 1)
        for label, flags in VARIANTS + [('wsl26.00 -mx9', None)]:
            arc = work / f'{name}.7z'
            t = time.perf_counter()
            if flags is None:
                subprocess.run(['wsl.exe', '-e', 'sh', '-c',
                                f"cd '{wslpath(root)}' && 7z a -t7z -mx9 -mmt1 -bd -bso0 -bsp0 '{wslpath(arc)}' '*'"],
                               check=True)
                lst = subprocess.run(['wsl.exe', '-e', '7z', 'l', '-slt', wslpath(arc)], capture_output=True,
                                     text=True, check=True).stdout
            else:
                subprocess.run([Z7, 'a', '-t7z', '-mx9', '-mmt1', *flags, '-bd', '-bso0', '-bsp0', str(arc.resolve()),
                                '*'], cwd=root, check=True)
                lst = subprocess.run([Z7, 'l', '-slt', str(arc)], capture_output=True, text=True, check=True).stdout
            wall = round(time.perf_counter() - t, 1)
            row = dict(tree=name, cell=label, archive_bytes=arc.stat().st_size, wall_s=wall, methods=methods(lst),
                       blocks=int(re.search(r'Blocks = (\d+)', lst).group(1)) if 'Blocks =' in lst else None)
            res.write(json.dumps(row) + '\n'); res.flush(); print(json.dumps(row), flush=True)
            arc.unlink()
work.rmdir()
```

## z7round2.py

```python
"""DEV-042 round 2: 7-Zip 23.01 (same binary as India) vs dictionary size, and xz with a 256 MiB dictionary.
python -I z7round2.py REPO OUT_JSONL WORK TREE_NAME=WINDIR ...
"""
import importlib.util, json, re, subprocess, sys, time
from pathlib import Path

repo, out, work = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
spec = importlib.util.spec_from_file_location('eb', repo / 'research/scripts/external_baseline.py')
eb = importlib.util.module_from_spec(spec); spec.loader.exec_module(eb)
Z2301 = str(Path(__file__).with_name('z7-2301') / 'root/usr/lib/7zip/7z')
work.mkdir(parents=True, exist_ok=False)


def wsl(p):
    p = str(Path(p).resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]


def sh(cmd):
    t = time.perf_counter()
    subprocess.run(['wsl.exe', '-e', 'sh', '-c', cmd], check=True)
    return round(time.perf_counter() - t, 1)


with open(out, 'a', encoding='utf-8') as res:
    def emit(**row):
        res.write(json.dumps(row) + '\n'); res.flush(); print(json.dumps(row), flush=True)
    v = subprocess.run(['wsl.exe', '-e', 'xz', '--version'], capture_output=True, text=True).stdout.split('\n')[0]
    emit(tool_versions=dict(xz_wsl=v, sevenzip_2301=wsl(Z2301)))
    for spec_ in sys.argv[4:]:
        name, root = spec_.split('=', 1)
        root = Path(root).resolve()
        arc = work / f'{name}.7z'
        for label, flags in [('wsl23.01 -mx9', ''), ('wsl23.01 -mx9 -md=256m', '-md=256m'),
                             ('wsl23.01 -mx9 -md=256m -mf=off', '-md=256m -mf=off')]:
            wall = sh(f"cd '{wsl(root)}' && '{wsl(Z2301)}' a -t7z -mx9 -mmt1 {flags} -bd -bso0 -bsp0 '{wsl(arc)}' '*'")
            lst = subprocess.run(['wsl.exe', '-e', wsl(Z2301), 'l', '-slt', wsl(arc)], capture_output=True, text=True,
                                 check=True).stdout
            ms = {}
            for m in re.findall(r'Method = (.*)', lst):
                ms[m] = ms.get(m, 0) + 1
            emit(tree=name, cell=label, archive_bytes=arc.stat().st_size, wall_s=wall, methods=ms)
            arc.unlink()
        tar = work / f'{name}.tar'
        eb.write_tar(root, eb.listing(root), tar)
        for label, opt in [('tar | xz -9e -T1 (WSL)', '-9e'),
                           ('tar | xz lzma2 preset=9e dict=256MiB -T1 (WSL)', '--lzma2=preset=9e,dict=256MiB')]:
            x = work / f'{name}.tar.xz'
            wall = sh(f"xz -T1 {opt} -c '{wsl(tar)}' > '{wsl(x)}'")
            emit(tree=name, cell=label, archive_bytes=x.stat().st_size, wall_s=wall, tar_bytes=tar.stat().st_size)
            x.unlink()
        tar.unlink()
work.rmdir()
```
