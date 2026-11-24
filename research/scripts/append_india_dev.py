"""DEV-017 fixed-host public dev pilot; Python stdlib, Linux Release tools.

Inputs are selected by committed dev TSVs. Raw archives are hash-checked, only
selected dev files are materialized. No holdout listings are read. Results are
append-only JSONL checkpoints; restart with a new run rather than mixing samples.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import statistics
import subprocess
import tarfile
import time
import urllib.request
import zipfile


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def download(url, path, expected):
    path = Path(path)
    if path.exists():
        raise FileExistsError(path)
    with urllib.request.urlopen(url, timeout=120) as response, path.open('xb') as out:
        shutil.copyfileobj(response, out, 1024 * 1024)
    if sha(path) != expected:
        raise ValueError(f'archive hash mismatch: {path.name}')


def rows(path):
    result = []
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        rel, size, digest, group, batch = line.split('\t')
        pure = Path(rel)
        if pure.is_absolute() or '..' in pure.parts or '\\' in rel:
            raise ValueError('unsafe input listing')
        result.append(dict(path=rel, size=int(size), sha256=digest, group=group, batch=batch))
    return result


def prepare_data(input_dir, work, results, config):
    downloads = work / 'downloads'
    downloads.mkdir()
    trees = {}
    report = {}
    for pool in config['pools']:
        listing = input_dir / f'dev-{pool}.tsv'
        selected = rows(listing)
        root = work / 'data' / pool
        root.mkdir(parents=True)
        if pool == 'linux-src':
            archive = downloads / 'linux-6.12.tar.xz'
            download('https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz', archive,
                     'b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb')
            wanted = {'linux-6.12/' + r['path'].removeprefix('linux/'): r for r in selected}
            with tarfile.open(archive, 'r|xz') as source:
                for member in source:
                    if member.name not in wanted:
                        continue
                    row = wanted.pop(member.name)
                    if not member.isfile() or member.size != row['size']:
                        raise ValueError('unexpected tar member')
                    dest = root / row['path']
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as src, dest.open('xb') as out:
                        shutil.copyfileobj(src, out)
            if wanted:
                raise ValueError('missing Linux source dev files')
        elif pool == 'wiki-text':
            archive = downloads / 'enwik8.zip'
            download('https://mattmahoney.net/dc/enwik8.zip', archive,
                     '547994d9980ebed1288380d652999f38a14fe291a6247c157c3d33d4932534bc')
            wanted = {r['path']: r for r in selected}
            # Only dev segments (0..49) are materialized/read for experimentation.
            with zipfile.ZipFile(archive) as source, source.open('enwik8') as stream:
                for index in range(50):
                    data = stream.read(1000000)
                    rel = f'enwik8/part-{index:03d}.xml'
                    if rel not in wanted:
                        continue
                    dest = root / rel
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(data)
            if any(int(r['path'].split('part-')[1][:3]) >= 50 for r in selected):
                raise ValueError('dev listing unexpectedly includes holdout')
        else:
            raise ValueError('only the two authorized public dev pools are supported')
        for row in selected:
            path = root / row['path']
            if path.stat().st_size != row['size'] or sha(path) != row['sha256']:
                raise ValueError(f'input hash mismatch: {row["path"]}')
        report[pool] = dict(files=len(selected), bytes=sum(r['size'] for r in selected),
                            listing_sha256=sha(listing), archive_sha256=sha(archive))
        trees[pool] = (root, selected)
        # Reconstructible downloaded originals need not consume disk during timing.
        archive.unlink()
    save(results / 'inputs-verified.json', report)
    return trees


def benchmark(input_dir, work, results, tool, cli, config, trees):
    selected_cpu = min(os.sched_getaffinity(0))
    raw = results / 'raw.jsonl'
    if raw.exists():
        raise FileExistsError('refuse to mix measurements with a previous attempt')
    logdir = results / 'commands'
    logdir.mkdir()
    all_records = []

    def call(label, args, meta, expected_json=True):
        index = len(all_records)
        affinity = lambda: os.sched_setaffinity(0, {selected_cpu})
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.perf_counter()
        proc = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                              timeout=1800, preexec_fn=affinity)
        elapsed = (time.perf_counter() - start) * 1000
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        stem = f'{index:04d}-{label}'
        (logdir / (stem + '.stdout')).write_text(proc.stdout, encoding='utf-8')
        (logdir / (stem + '.stderr')).write_text(proc.stderr, encoding='utf-8')
        rec = dict(index=index, label=label, **meta, argv=[str(a) for a in args],
                   returncode=proc.returncode, process_wall_ms=elapsed,
                   process_cpu_ms=((after.ru_utime + after.ru_stime) -
                                   (before.ru_utime + before.ru_stime)) * 1000,
                   loadavg=list(os.getloadavg()), cpu=selected_cpu)
        if proc.returncode == 0 and expected_json:
            rec['result'] = json.loads(proc.stdout)
        all_records.append(rec)
        with raw.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(rec, ensure_ascii=False) + '\n')
        print(json.dumps(dict(progress=stem, **meta, wall_ms=round(elapsed))), flush=True)
        if proc.returncode:
            raise RuntimeError(f'{stem} failed: {proc.returncode}; see retained logs')
        return rec.get('result', {})

    def verify(path, tree, meta):
        result = call('verify', [tool, 'verify', path, tree], meta)
        if not result.get('byte_exact'):
            raise AssertionError('independent verification did not confirm byte-exact roundtrip')

    def pack(mode, plan, tree, out, meta):
        result = call('pack', [tool, 'pack', mode, plan, tree, out, 'off'], meta)
        verify(out, tree, meta)
        return result

    def search(method, start, tree, size, seed, policy, meta):
        multiple = config['initial_budget_multiple'] if meta['stage'] == 0 else config['search_budget_multiple']
        return call('search', [tool, 'search', method, start, tree, size * multiple, seed, policy], meta)

    for rep in range(config['repetitions']):
        pools = config['pools'][rep % len(config['pools']):] + config['pools'][:rep % len(config['pools'])]
        for pool in pools:
            base, selected = trees[pool]
            run = work / f'rep{rep}-{pool}'
            run.mkdir()
            cumulative = run / 'cumulative'
            cumulative.mkdir()
            seed = config['prng_seeds'][rep]
            previous = {}
            best_plan = None
            for stage, batch in enumerate(['initial', 'append1', 'append2', 'append3']):
                added = run / batch
                added.mkdir()
                for row in selected:
                    if row['batch'] != batch:
                        continue
                    for root in (added, cumulative):
                        dest = root / row['path']
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        os.link(base / row['path'], dest)
                if not any(p.is_file() for p in added.rglob('*')):
                    raise ValueError(f'empty batch: {pool}/{batch}')
                # Explicit hot-cache policy; consume bytes outside measured calls.
                size = 0
                for path in sorted(cumulative.rglob('*')):
                    if path.is_file():
                        with path.open('rb') as stream:
                            while data := stream.read(1024 * 1024):
                                size += len(data)
                meta = dict(rep=rep, pool=pool, stage=stage, input_bytes=size)
                if stage == 0:
                    found = search('seed', 'default', cumulative, size, seed, 'restart',
                                   dict(**meta, variant='initial-search'))
                    best_plan = initial_plan = found['best_plan']
                    for mode in ['seed', 'explicit', 'solid']:
                        out = run / f'{mode}-0.sadx'
                        pack(mode, initial_plan, cumulative, out, dict(**meta, variant=mode + '-initial'))
                        previous[mode] = out
                    continue
                actions = ['append-seed', 'append-explicit', 'solid-segment', 'solid-regroup',
                           'rebuild-seed', 'rebuild-solid', 'lzma6', 'seed-cold', 'seed-warm',
                           'coord-cold', 'coord-warm', 'new-generation']
                shift = rep % len(actions)
                actions = actions[shift:] + actions[:shift]
                next_previous = {}
                warm_results = {}
                for action in actions:
                    ctx = dict(**meta, variant=action)
                    out = run / f'{action}-{stage}.sarc'
                    if action in ('append-seed', 'append-explicit', 'solid-segment', 'solid-regroup'):
                        mode = 'seed' if action == 'append-seed' else 'explicit' if action == 'append-explicit' else 'solid'
                        how = 'regroup' if action == 'solid-regroup' else 'segment'
                        call('append', [tool, 'append', previous[mode], added, out, how], ctx)
                        verify(out, cumulative, ctx)
                        if action != 'solid-regroup':
                            next_previous[mode] = out
                    elif action.startswith('rebuild-'):
                        pack(action.removeprefix('rebuild-'), initial_plan, cumulative, out, ctx)
                    elif action == 'lzma6':
                        call('compress', [cli, 'compress', cumulative, '-o', out, '--codec', 'lzma2', '--level', 6], ctx, False)
                        call('verify-cli', [cli, 'verify', out], ctx, False)
                        extracted = run / f'extracted-{stage}'
                        call('extract-cli', [cli, 'extract', out, '-o', extracted], ctx, False)
                        expected = {p.relative_to(cumulative).as_posix(): sha(p) for p in cumulative.rglob('*') if p.is_file()}
                        actual = {p.relative_to(extracted).as_posix(): sha(p) for p in extracted.rglob('*') if p.is_file()}
                        if actual != expected:
                            raise AssertionError('LZMA2 independent roundtrip mismatch')
                        shutil.rmtree(extracted)
                    elif action == 'new-generation':
                        added_size = sum(r['size'] for r in selected if r['batch'] == batch)
                        found = search('seed', 'default', added, added_size, seed, 'restart', ctx)
                        call('append', [tool, 'append', previous['seed'], added, out, '--plan', found['best_plan']], ctx)
                        verify(out, cumulative, ctx)
                    else:
                        method, policy = action.split('-')
                        found = search(method, best_plan if policy == 'warm' else 'default', cumulative,
                                       size, seed, 'local' if policy == 'warm' else 'restart', ctx)
                        if policy == 'warm':
                            warm_results[method] = found['best_plan']
                        pack('seed' if method == 'seed' else 'explicit', found['best_plan'], cumulative, out, ctx)
                    # Retain final sizes/hashes while keeping artifact disk use bounded.
                    if out.exists():
                        with (results / 'artifacts.jsonl').open('a', encoding='utf-8') as stream:
                            stream.write(json.dumps(dict(**ctx, bytes=out.stat().st_size, sha256=sha(out))) + '\n')
                        if out not in next_previous.values():
                            out.unlink()
                for path in previous.values():
                    path.unlink()
                previous = next_previous
                best_plan = warm_results['seed']
            for path in previous.values():
                path.unlink()
            shutil.rmtree(run)
    save(results / 'pilot-complete.json', dict(commands=len(all_records), byte_exact=True,
         scope='two public dev pools; exploratory only; no holdout conclusions'))
    analyze(results)


def analyze(results):
    records = [json.loads(line) for line in (results / 'raw.jsonl').read_text().splitlines()]
    artifacts = [json.loads(line) for line in (results / 'artifacts.jsonl').read_text().splitlines()]
    grouped = {}
    for rec in records:
        if rec['label'] in ('pack', 'append', 'search', 'compress'):
            key = (rec['pool'], rec['stage'], rec['variant'])
            grouped.setdefault(key, []).append(rec)
    summary = []
    for key, group in sorted(grouped.items()):
        sizes = [a['bytes'] for a in artifacts if (a['pool'], a['stage'], a['variant']) == key]
        per_rep = {}
        for r in group:
            per_rep[r['rep']] = per_rep.get(r['rep'], 0) + r['process_wall_ms']
        summary.append(dict(pool=key[0], stage=key[1], variant=key[2],
                            repetitions=len(per_rep), wall_median_ms=statistics.median(per_rep.values()),
                            wall_min_ms=min(per_rep.values()), wall_max_ms=max(per_rep.values()),
                            bytes=sizes, timing='sum of search+write where applicable; verification excluded'))
    save(results / 'summary.json', summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--tool', type=Path)
    parser.add_argument('--cli', type=Path)
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    if args.analyze_only:
        analyze(args.results)
        return
    config = json.loads((args.input / 'config.json').read_text())
    save(args.results / 'config.json', config)
    save(args.results / 'environment.json', dict(platform=platform.platform(), python=platform.python_version(),
         affinity=sorted(os.sched_getaffinity(0)), initial_load=os.getloadavg(),
         tool_sha256=sha(args.tool), cli_sha256=sha(args.cli),
         memory=Path('/proc/meminfo').read_text(), cpu=Path('/proc/cpuinfo').read_text(),
         os_release=Path('/etc/os-release').read_text()))
    trees = prepare_data(args.input, args.work, args.results, config)
    benchmark(args.input, args.work, args.results, args.tool, args.cli, config, trees)


if __name__ == '__main__':
    main()
