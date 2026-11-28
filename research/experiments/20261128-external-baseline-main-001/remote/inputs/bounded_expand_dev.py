# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-023: one-batch bounded-frame histories on new dev trees, with balanced repeats.

A unit is (input class, added batch). Per unit and frame limit:
  preparation (separate): seed parent pack + verify, explicit parent pack + verify, payload-compare;
  rounds: rotated cap order, alternating fresh / append; every output verified, then
  access-profile; the first output of each cap/operation also gets payload-fault-profile.
Parents are immutable and never replaced by repeat outputs.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import time

CAPS = [1024**3, 32 * 1024**2, 64 * 1024**2, 128 * 1024**2]
BATCH_KEYS = ('added10', 'added40')
DISK_GATE = 1900 * 1024**2
MAX_FILES = 100000


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')


def load(root, manifest, verify_content=True):
    """Rows of a 6-column manifest: rel, size, sha256, origin, added10, added40."""
    rows, seen = [], set()
    for line in Path(manifest).read_text(encoding='utf-8').splitlines():
        rel, size, digest, origin, a10, a40 = line.split('\t')
        p = PurePosixPath(rel)
        if (p.is_absolute() or '..' in p.parts or p.as_posix() != rel or '\\' in rel or ':' in rel
                or rel in seen or a10 not in '01' or a40 not in '01' or len(a10) != 1 or len(a40) != 1):
            raise ValueError('unsafe, duplicate or malformed manifest row')
        size = int(size)
        if size < 0 or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('invalid size or SHA256')
        current = Path(root)
        for part in p.parts:
            current = current / part
            if current.is_symlink() or (hasattr(current, 'is_junction') and current.is_junction()):
                raise ValueError('linked input is not accepted')
        if verify_content and (not current.is_file() or current.stat().st_size != size or sha(current) != digest):
            raise ValueError('input differs from manifest: ' + rel)
        seen.add(rel)
        rows.append(dict(path=rel, size=size, sha256=digest, origin=origin, added10=a10 == '1', added40=a40 == '1'))
    if not rows or len(rows) > MAX_FILES:
        raise ValueError('empty or oversized manifest')
    if verify_content:
        present = {p.relative_to(root).as_posix() for p in Path(root).rglob('*') if p.is_file() or p.is_symlink()}
        if present != seen:
            raise ValueError('input tree holds files outside the manifest')
    return rows


def disk_bytes(root):
    total = 0
    for folder, _dirs, names in os.walk(root, followlinks=False):
        for name in names:
            try:
                info = os.lstat(os.path.join(folder, name))
            except FileNotFoundError:
                continue
            if not os.path.islink(os.path.join(folder, name)):
                total += info.st_size
    return total


def remove_owned(path, owner):
    path, owner = Path(path), Path(owner).resolve()
    if path.is_symlink() or path.resolve().parent != owner:
        raise ValueError('unexpected owned cleanup root')
    if any(p.is_symlink() for p in path.rglob('*')):
        raise ValueError('link in owned cleanup tree')
    shutil.rmtree(path)


def prefix_matches(parent, child):
    with open(parent, 'rb') as old, open(child, 'rb') as new:
        while True:
            block = old.read(1024**2)
            if not block:
                return True
            if new.read(len(block)) != block:
                return False


def link_tree(data, rows, target, chosen):
    target.mkdir()
    for row in rows:
        if chosen(row):
            dest = target / row['path']
            dest.parent.mkdir(parents=True, exist_ok=True)
            os.link(data / row['path'], dest)


def run_unit(tool, data, manifest, cls, batch_key, work, results, task_root, caps=None, rounds=None,
             sample_count=12, pin_cpu=True, disk_gate=DISK_GATE):
    if batch_key not in BATCH_KEYS:
        raise ValueError('unknown batch')
    tool, data = Path(tool).resolve(strict=True), Path(data).resolve(strict=True)
    rows = load(data, manifest)
    caps = list(caps or CAPS)
    rounds = rounds or len(caps)
    if len(caps) < 2 or len(set(caps)) != len(caps) or any(not 1 <= c <= 1024**3 for c in caps) or rounds != len(caps):
        raise ValueError('invalid caps / rounds')
    if max(r['size'] for r in rows) > min(caps):
        raise ValueError('an input exceeds the smallest pre-registered frame limit')
    added_rows = [r for r in rows if r[batch_key]]
    if not added_rows or len(added_rows) == len(rows):
        raise ValueError('added batch must be a proper nonempty subset')
    work.mkdir()
    results.mkdir()
    logs = results / 'commands'
    logs.mkdir()
    affinity = {min(os.sched_getaffinity(0))} if pin_cpu and hasattr(os, 'sched_getaffinity') else None
    total = sum(r['size'] for r in rows)
    added_bytes = sum(r['size'] for r in added_rows)
    orders = [caps[i:] + caps[:i] for i in range(rounds)]
    write_json(results / 'config.json', dict(cls=cls, batch=batch_key, manifest_sha256=sha(manifest),
        tool_sha256=sha(tool), runner_sha256=sha(Path(__file__)), files=len(rows), bytes=total,
        added_files=len(added_rows), added_bytes=added_bytes, added_ratio=added_bytes / total,
        parent_files=len(rows) - len(added_rows), parent_bytes=total - added_bytes, frame_limits=caps,
        orders=orders, rounds=rounds, sample_count=sample_count,
        operation_order='fresh then append on even rounds; reverse on odd rounds',
        affinity=sorted(affinity) if affinity else None, mode='seed (explicit only in preparation control)',
        plan='single group, Zstd 9, no dictionary, redundancy off',
        cache='prewarm command input tree; verify before access; no global cache dropping',
        scope='existing public dev extension; warm cache; not a holdout'))
    records, artifacts, faults = [], [], {}

    def call(args, label, tree, meta):
        for p in sorted(tree.rglob('*')):
            if p.is_file():
                with p.open('rb') as stream:
                    while stream.read(1024**2):
                        pass
        used = disk_bytes(task_root)
        if used + total + len(rows) * 1024 + 16 * 1024**2 >= disk_gate:
            raise RuntimeError('retain output headroom')

        def fixed_cpu():
            os.sched_setaffinity(0, affinity)
        start = time.perf_counter()
        proc = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True, encoding='utf-8',
                              timeout=1800, preexec_fn=fixed_cpu if affinity else None)
        wall = (time.perf_counter() - start) * 1000
        stem = f'{len(records):03d}-{label}'
        (logs / (stem + '.stdout')).write_text(proc.stdout, encoding='utf-8')
        (logs / (stem + '.stderr')).write_text(proc.stderr, encoding='utf-8')
        item = dict(index=len(records), label=label, argv=list(map(str, args)), meta=meta,
                    returncode=proc.returncode, process_wall_ms=wall, disk_before=used, disk_after=disk_bytes(task_root))
        if proc.returncode == 0:
            item['result'] = json.loads(proc.stdout)
        records.append(item)
        with (results / 'raw.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(item, ensure_ascii=False) + '\n')
        print(json.dumps({k: item[k] for k in ['index', 'label', 'returncode', 'process_wall_ms']}), flush=True)
        if proc.returncode or item['disk_after'] >= disk_gate:
            raise RuntimeError(label + ': command or disk gate failed; preserve evidence')
        return item['result']

    def checked(path, result, label, tree, cap, generations, meta, parent=None):
        check = call(['verify', path, tree], label + '-verify', tree, dict(meta, kind='verify'))
        if not check['byte_exact'] or check['generations'] != generations:
            raise AssertionError(label + ': independent verification failed')
        frames = result['frame_raw_bytes']
        if (path.stat().st_size != result['container_bytes'] or result['max_frame_raw_bytes'] > cap
                or result['frame_count'] != len(frames) or max(frames) != result['max_frame_raw_bytes']
                or result['frame_limit_bytes'] != cap):
            raise AssertionError(label + ': hard cap / physical size failed')
        if parent and (not result['old_is_prefix'] or sha(parent['path']) != parent['sha256']
                       or not prefix_matches(parent['path'], path) or result['old_plaintext_bytes_recompressed'] != 0
                       or result['old_archive_bytes_read'] != parent['bytes']
                       or result['old_archive_bytes_copied'] != parent['bytes']):
            raise AssertionError(label + ': parent changed, not a prefix, or old plaintext recompressed')
        digest = sha(path)
        obj = dict(label=label, meta=meta, cap=cap, bytes=path.stat().st_size, sha256=digest, result=result)
        artifacts.append(obj)
        write_json(results / 'archives.json', artifacts)
        return dict(path=path, sha256=digest, bytes=obj['bytes'])

    final = data  # exactly the manifest rows (checked by load)
    parent_tree = work / 'parent-input'
    link_tree(data, rows, parent_tree, lambda r: not r[batch_key])
    added = work / 'added'
    link_tree(data, rows, added, lambda r: r[batch_key])

    parents = {}
    for cap in caps:
        reps = {}
        for mode in ['seed', 'explicit']:
            label = f'prepare-{cap}-{mode}'
            meta = dict(phase='prepare', kind='encode', cap=cap, mode=mode)
            path = work / f'parent-{cap}-{mode}.sadx'
            result = call(['bounded-pack', mode, cap, parent_tree, path], label, parent_tree, meta)
            reps[mode] = checked(path, result, label, parent_tree, cap, 1, meta)
        cmp = call(['payload-compare', reps['seed']['path'], reps['explicit']['path']], f'prepare-{cap}-payload',
                   parent_tree, dict(phase='prepare', kind='payload-compare', cap=cap))
        if not cmp['equal']:
            raise AssertionError('seed / explicit payload differs')
        reps['explicit']['path'].unlink()
        parents[cap] = reps['seed']
    write_json(results / 'parents.json', {str(c): {k: v for k, v in p.items() if k != 'path'} for c, p in parents.items()})
    remove_owned(parent_tree, work)

    for r, order in enumerate(orders):
        for position, cap in enumerate(order):
            operations = ['fresh', 'append'] if r % 2 == 0 else ['append', 'fresh']
            for op_position, operation in enumerate(operations):
                label = f'round-{r}-{cap}-{operation}'
                meta = dict(phase='repeat', kind='encode', round=r, position=position,
                            operation_position=op_position, operation=operation, cap=cap)
                path = work / 'output.sadx'
                if operation == 'fresh':
                    args, tree = ['bounded-pack', 'seed', cap, final, path], final
                else:
                    args, tree = ['bounded-append', cap, parents[cap]['path'], added, path], added
                result = call(args, label, tree, meta)
                out = checked(path, result, label, final, cap, 1 if operation == 'fresh' else 2, meta,
                              parents[cap] if operation == 'append' else None)
                access = call(['access-profile', path, final, sample_count], label + '-access', final,
                              dict(meta, kind='access'))
                if (not access['byte_exact'] or access['sample_count'] < 1
                        or any(s['decoded_bytes'] > cap or s['decoded_bytes'] < s['requested_bytes']
                               for s in access['samples'])):
                    raise AssertionError(label + ': access bound / verification failed')
                key = (cap, operation)
                if key not in faults:
                    fault = call(['payload-fault-profile', path, final], label + '-fault', final, dict(meta, kind='fault'))
                    if fault['wrong_bytes_accepted'] != 0 or fault['outputs_written'] != 0 or not fault['verified_source']:
                        raise AssertionError(label + ': fault diagnostic accepted wrong bytes')
                    faults[key] = dict(sha256=out['sha256'], label=label)
                elif faults[key]['sha256'] != out['sha256']:
                    raise AssertionError(label + ': repeat archive SHA differs from the first output')
                path.unlink()
    for cap, parent in parents.items():
        if sha(parent['path']) != parent['sha256']:
            raise AssertionError('parent mutated')
        parent['path'].unlink()
    remove_owned(added, work)
    if load(data, manifest) != rows:
        raise AssertionError('input tree changed during the unit')
    n = len(caps)
    expected = 5 * n + 3 * 2 * n * rounds + 2 * n
    if len(records) != expected or len(artifacts) != 2 * n + 2 * n * rounds:
        raise AssertionError('incomplete pre-registered unit')
    complete = dict(cls=cls, batch=batch_key, commands=len(records), archives_verified=len(artifacts),
                    repeat_encodes=2 * n * rounds, access_profiles=2 * n * rounds, fault_profiles=len(faults),
                    fault_reuse='later outputs with the same SHA reuse the first fault profile',
                    all_byte_exact=True, all_parents_immutable=True, repeat_sha_stable=True)
    write_json(results / 'complete.json', complete)
    return complete
