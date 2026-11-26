# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-031 cross-CPU byte identity: run one fixed set of deterministic writes with one binary and
record the SHA256 of every archive, so runs on different hosts can be compared field by field.

  cross_cpu_check.py run TOOL DATA_DIR WORK_DIR OUT_JSON     (DATA_DIR holds linux/ and silesia/)
  cross_cpu_check.py compare LEFT_JSON RIGHT_JSON

Only bytes are compared; timings are kept for reference and never compared. Every archive is
verified byte for byte against its input before its hash is recorded.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
FIXTURES = HERE if (HERE / 'bounded_fixture.py').exists() else HERE.parent / 'tools'  # bundle or repository
RECIPES = ['solid/zstd-1/g1-r0/frame-16MiB', 'solid/zstd-3/g1-r0/frame-16MiB', 'solid/zstd-9/g1-r0/frame-16MiB',
           'solid/zstd-19/g1-r0/frame-16MiB', 'solid/lzma2-0/g1-r0/frame-16MiB', 'solid/lzma2-6/g1-r0/frame-16MiB',
           'solid/lzma2-9/g1-r0/frame-16MiB', 'solid/zstd-9/g2-r1/frame-whole', 'solid/lzma2-6/g2-r1/frame-whole']
CHAINS = ['solid/zstd-9/g1-r0/frame-32MiB', 'solid/lzma2-9/g1-r0/frame-32MiB']
TIMING = ('_ms', 'peak_memory')


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def tree_digest(root):
    # Explicit case-sensitive component order: Path ordering is case-insensitive on Windows, which
    # gave a different digest for the same tree. This is the order PosixPath used on Linux.
    h = hashlib.sha256()
    rels = [p.relative_to(root).as_posix() for p in Path(root).rglob('*') if p.is_file()]
    for rel in sorted(rels, key=lambda r: PurePosixPath(r).parts):
        h.update(rel.encode() + b'\0' + sha(Path(root) / rel).encode() + b'\n')
    return h.hexdigest()


class Tool:
    def __init__(self, path):
        self.path, self.seconds = path, {}

    def __call__(self, label, *args):
        start = time.perf_counter()
        r = subprocess.run([str(self.path), *map(str, args)], capture_output=True, text=True, encoding='utf-8')
        self.seconds[label] = time.perf_counter() - start
        if r.returncode:
            raise RuntimeError(f'{label}: exit {r.returncode}: {r.stderr[-800:]}')
        return json.loads(r.stdout) if r.stdout.strip() else None


def bytes_only(value):
    if isinstance(value, dict):
        return {k: bytes_only(v) for k, v in value.items() if not any(t in k for t in TIMING)}
    if isinstance(value, list):
        return [bytes_only(v) for v in value]
    return value


def split(root, old, new):
    """70 / 30 by a path hash that does not depend on the data; hard links, no copies."""
    for p in sorted(Path(root).rglob('*')):
        if p.is_file():
            rel = p.relative_to(root).as_posix()
            side = new if int(hashlib.sha256(('dev031:' + rel).encode()).hexdigest(), 16) % 10 < 3 else old
            dest = side / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            os.link(p, dest)


def run(tool_path, data, work, out_json):
    tool, archives, probes = Tool(tool_path), {}, {}
    work.mkdir()
    subprocess.run([sys.executable, '-I', FIXTURES / 'bounded_fixture.py', work / 'small'], check=True)
    subprocess.run([sys.executable, '-I', FIXTURES / 'recipe_fixture.py', 'big', work / 'big'], check=True)
    trees = {'small-all': work / 'small/all', 'big': work / 'big', 'linux': data / 'linux', 'silesia': data / 'silesia'}
    inputs = {name: tree_digest(t) for name, t in trees.items()}

    def keep(label, archive, tree):
        tool(label + '-verify', 'verify', archive, tree)
        archives[label] = dict(sha256=sha(archive), bytes=archive.stat().st_size)
        archive.unlink()

    for name, tree in trees.items():
        for rec in RECIPES:
            label = f'{name}|pack|{rec}'
            out = work / 'a.sadx'
            tool(label, 'recipe-pack', 'seed', rec, tree, out)
            keep(label, out, tree)
        out = work / 'a.sadx'
        tool(f'{name}|pack-explicit', 'recipe-pack', 'explicit', RECIPES[2], tree, out)
        keep(f'{name}|pack-explicit|{RECIPES[2]}', out, tree)
    old, new = work / 'old', work / 'new'
    split(data / 'linux', old, new)
    for rec in CHAINS:
        first, grown, one = work / 'g0.sadx', work / 'g1.sadx', work / 'one.sadx'
        tool(f'chain|{rec}|pack', 'recipe-pack', 'seed', rec, old, first)
        tool(f'chain|{rec}|append', 'recipe-append', first, new, grown)
        archives[f'linux|chain-g0|{rec}'] = dict(sha256=sha(first), bytes=first.stat().st_size)
        first.unlink()
        tool(f'chain|{rec}|consolidate', 'consolidate', grown, one)
        keep(f'linux|chain-g1|{rec}', grown, data / 'linux')
        keep(f'linux|consolidated|{rec}', one, data / 'linux')
    for level in (3, 9):
        probes[f'linux|drift-probe|zstd-{level}'] = bytes_only(tool(f'probe-{level}', 'drift-probe', old, new, level))
    shutil.rmtree(work)
    report = dict(schema='seedarc.cross-cpu.r0', host=platform.node(), machine=platform.machine(),
                  cpu=next((l.split(':', 1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines()
                            if l.startswith('model name')), None) if Path('/proc/cpuinfo').exists() else platform.processor(),
                  cpu_flags_avx2=('avx2' in Path('/proc/cpuinfo').read_text()) if Path('/proc/cpuinfo').exists() else None,
                  tool_sha256=sha(tool_path), script_sha256=sha(__file__), inputs=inputs, archives=archives,
                  probes=probes, seconds=tool.seconds)
    Path(out_json).write_text(json.dumps(report, indent=1, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(dict(archives=len(archives), probes=len(probes))))


def compare(left, right):
    a, b = (json.loads(Path(p).read_text(encoding='utf-8')) for p in (left, right))
    diff = dict(
        tool_same=a['tool_sha256'] == b['tool_sha256'], script_same=a['script_sha256'] == b['script_sha256'],
        inputs_differ=sorted(k for k in a['inputs'].keys() | b['inputs'].keys() if a['inputs'].get(k) != b['inputs'].get(k)),
        archives_differ=sorted(k for k in a['archives'].keys() | b['archives'].keys()
                               if a['archives'].get(k) != b['archives'].get(k)),
        probes_differ=sorted(k for k in a['probes'].keys() | b['probes'].keys() if a['probes'].get(k) != b['probes'].get(k)),
        archives=len(a['archives']), hosts=[(a['host'], a['cpu'], a['cpu_flags_avx2']), (b['host'], b['cpu'], b['cpu_flags_avx2'])])
    diff['identical'] = not (diff['inputs_differ'] or diff['archives_differ'] or diff['probes_differ'])
    print(json.dumps(diff, indent=1, ensure_ascii=False))
    return 0 if diff['identical'] else 1


if __name__ == '__main__':
    if len(sys.argv) == 6 and sys.argv[1] == 'run':
        run(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve(), Path(sys.argv[4]).resolve(), Path(sys.argv[5]))
    elif len(sys.argv) == 4 and sys.argv[1] == 'compare':
        sys.exit(compare(sys.argv[2], sys.argv[3]))
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
