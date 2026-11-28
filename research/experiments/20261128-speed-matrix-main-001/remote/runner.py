"""Remote per-request dispatcher and persistent task runner; Python stdlib only."""
import base64
import datetime
import fcntl
import hashlib
import json
import os
import stat
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import time

BASE = Path('/home/sc/long-tasks')
PATTERN = re.compile(r'^[0-9]{8}T[0-9]{6}Z-[a-f0-9]{8}$')


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def atomic(path, value):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, indent=2), encoding='utf-8')
    tmp.replace(path)


def job_path(task):
    if not PATTERN.fullmatch(task):
        raise ValueError('Invalid task ID')
    if BASE.is_symlink() or (BASE.exists() and BASE.resolve() != BASE):
        raise ValueError('Unexpected task root')
    path = BASE / task
    if path.is_symlink() or path.resolve() != path or not path.is_dir():
        raise ValueError('Task not found or unsafe path')
    return path


def unit_status(task):
    r = subprocess.run(['systemctl', 'show', 'injob-' + task,
                        '--property=LoadState,ActiveState,SubState,Result,ExecMainStatus,MemoryMax,MemorySwapMax,TasksMax,CPUQuotaPerSecUSec,User,NoNewPrivileges'],
                       capture_output=True, text=True, timeout=15)
    return dict(l.split('=', 1) for l in r.stdout.splitlines() if '=' in l)


def running(status):
    return status.get('ActiveState') in ('activating', 'deactivating', 'reloading') or (
        status.get('ActiveState') == 'active' and status.get('SubState') != 'exited')


def status(task):
    path = job_path(task)
    state = unit_status(task)
    result = json.loads((path / 'result.json').read_text()) if (path / 'result.json').exists() else None
    phase = 'running' if running(state) else ('finished' if result else 'unknown-or-not-started')
    # A runtime limit/OOM/reboot can stop the runner before result.json is written.
    return {'task_id': task, 'phase': phase, 'mode': json.loads((path / 'manifest.json').read_text()).get('mode', 'shared'), 'unit': state, 'result': result}


def require_stopped(task):
    st = status(task)
    if st['phase'] == 'running':
        raise ValueError('Task is still running; do not bundle or clean')
    if not st['result'] and st['unit'].get('LoadState') == 'not-found':
        raise ValueError('Completion unknown; diagnose before cleanup')
    return st


def dispatch(req, source):
    if subprocess.check_output(['hostname'], text=True).strip() != 'vps-in1':
        raise ValueError('Wrong host')
    op = req['op']
    if op == 'preflight':
        return {'hostname': subprocess.check_output(['hostname'], text=True).strip(),
                'free_disk_bytes': shutil.disk_usage('/home/sc').free,
                'sudo_available': subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode == 0,
                'tasks': [status(p.name) for p in BASE.iterdir() if PATTERN.fullmatch(p.name)] if BASE.exists() else []}
    if op == 'prepare':
        exclusive = req.get('exclusive', False)
        if not isinstance(exclusive, bool):
            raise ValueError('exclusive must be a boolean')
        task = req['id']
        if not PATTERN.fullmatch(task) or BASE.is_symlink():
            raise ValueError('Unsafe task root or ID')
        if shutil.disk_usage('/home/sc').free < 2 * 1024**3:
            raise ValueError('Less than 2GiB free; clean only owned temporary data first')
        BASE.mkdir(mode=0o700, exist_ok=True)
        path = BASE / task
        path.mkdir(mode=0o700)  # Never reuse/overwrite a task ID.
        for name in ['work', 'tmp', 'results', 'logs']:
            (path / name).mkdir(mode=0o700)
        (path / 'task.sh').write_bytes(base64.b64decode(req['script']))
        (path / 'runner.py').write_text(source, encoding='utf-8')
        (path / 'task.sh').chmod(0o600)
        (path / 'runner.py').chmod(0o600)
        for entry in req.get('inputs', []):
            rel = Path(entry['path'])
            if rel.is_absolute() or '..' in rel.parts or not rel.parts:
                raise ValueError('Unsafe input path')
            dest = path / 'work' / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(base64.b64decode(entry['data']))
            dest.chmod(0o600)
        atomic(path / 'manifest.json', {'task_id': task, 'created_at': now(),
               'script_sha256': hashlib.sha256((path / 'task.sh').read_bytes()).hexdigest(),
               'mode': 'exclusive' if exclusive else 'shared',
               'cpu_quota': 'infinity' if exclusive else '200%',
               'memory_max': 'infinity' if exclusive else '3G',
               'swap_max': 'infinity' if exclusive else '512M',
               'tasks_max': 'infinity' if exclusive else '256',
               'runtime_seconds': req['runtime'], 'log_limit_bytes': 64 * 1024**2,
               'disk_limit_bytes': 2 * 1024**3})
        return {'task_id': task, 'prepared': True, 'remote_path': str(path)}
    task = req['id']
    path = job_path(task)
    if op == 'start':
        with (BASE / '.start.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if (path / 'submitted.json').exists():
                raise ValueError('Already submitted; inspect status instead of retrying')
            manifest = json.loads((path / 'manifest.json').read_text())
            mode = manifest.get('mode', 'shared')
            if mode not in ('shared', 'exclusive'):
                raise ValueError('Unknown task mode')
            active_modes = []
            for p in BASE.iterdir():
                if PATTERN.fullmatch(p.name) and running(unit_status(p.name)):
                    # An unreadable/unknown manifest must block admission, never free a slot.
                    active_mode = json.loads((job_path(p.name) / 'manifest.json').read_text()).get('mode', 'shared')
                    if active_mode not in ('shared', 'exclusive'):
                        raise ValueError('Active task has unknown mode; inspect first')
                    active_modes.append(active_mode)
            if active_modes and (mode == 'exclusive' or 'exclusive' in active_modes):
                raise ValueError('Exclusive task requires an idle server and blocks all other tasks')
            if len(active_modes) >= 2:
                raise ValueError('Two shared tasks are already running')
            if subprocess.check_output(['hostname'], text=True).strip() != 'vps-in1':
                raise ValueError('Wrong host')
            manifest = json.loads((path / 'manifest.json').read_text())
            # Persist intent before RPC: an SSH timeout must never trigger automatic duplication.
            atomic(path / 'submitted.json', {'requested_at': now()})
            props = {'User': 'sc', 'Group': 'sc', 'WorkingDirectory': str(path / 'work'),
                     'CPUQuota': 'infinity' if mode == 'exclusive' else '200%',
                     'CPUWeight': '10', 'MemoryMax': 'infinity' if mode == 'exclusive' else '3G',
                     'MemorySwapMax': 'infinity' if mode == 'exclusive' else '512M',
                     'TasksMax': 'infinity' if mode == 'exclusive' else '256', 'Nice': '10', 'IOWeight': '10',
                     'NoNewPrivileges': 'yes', 'ProtectSystem': 'strict', 'ProtectHome': 'read-only',
                     'ReadWritePaths': str(path), 'PrivateTmp': 'yes', 'CapabilityBoundingSet': '',
                     'RestrictSUIDSGID': 'yes', 'KillMode': 'control-group', 'TimeoutStopSec': '15s',
                     'RuntimeMaxSec': str(manifest['runtime_seconds']),
                     'StandardOutput': 'null', 'StandardError': 'journal'}
            if mode == 'exclusive':
                # New transient units have no CPU quota unless explicitly set.
                props.pop('CPUQuota')
            args = ['sudo', '-n', 'systemd-run', '--quiet', '--unit=injob-' + task, '--service-type=exec']
            for k, v in props.items():
                args.extend(['--property', k + '=' + v])
            args += ['/usr/bin/python3', str(path / 'runner.py'), '--run', task]
            r = subprocess.run(args, capture_output=True, text=True, timeout=25)
            if r.returncode:
                atomic(path / 'submission-error.json', {'exit_code': r.returncode, 'message': r.stderr[-1500:]})
                raise RuntimeError('systemd submission failed; inspect submission-error.json; do not resubmit automatically')
            started = status(task)
            atomic(path / 'unit-start.json', started)
            return started
    if op == 'status':
        return status(task)
    if op == 'stop':
        subprocess.run(['sudo', '-n', 'systemctl', 'stop', 'injob-' + task], check=True, timeout=25)
        return status(task)
    if op == 'bundle':
        st = require_stopped(task)
        atomic(path / 'status.json', st)
        selected = [path / n for n in ['manifest.json', 'task.sh', 'runner.py', 'submitted.json',
                    'result.json', 'status.json', 'unit-start.json', 'submission-error.json', 'logs', 'results'] if (path / n).exists()]
        total = 0
        for p in selected:
            for f in ([p] + list(p.rglob('*')) if p.is_dir() else [p]):
                if f.is_symlink() or (not f.is_file() and not f.is_dir()):
                    raise ValueError('Nonregular result/log entry; do not follow links')
                if f.is_file():
                    total += f.stat().st_size
        if total > 512 * 1024**2:
            raise ValueError('Artifacts exceed 512MiB; arrange explicit larger transfer')
        archive = path / 'bundle.tar.gz'
        with tarfile.open(archive, 'w:gz', dereference=False) as tar:
            for p in selected:
                tar.add(p, arcname=p.name)
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        atomic(path / 'bundle.json', {'sha256': digest})
        return {'path': str(archive), 'sha256': digest, 'bytes': archive.stat().st_size}
    if op == 'clean':
        require_stopped(task)
        expected = json.loads((path / 'bundle.json').read_text())['sha256']
        if req['sha256'] != expected:
            raise ValueError('Local verified receipt does not match bundle')
        for name in ['work', 'tmp']:
            target = path / name
            if target.is_symlink() or target.resolve() != target or target.parent != path:
                raise ValueError('Unsafe cleanup target')
            if target.exists():
                shutil.rmtree(target)  # Only task-owned transient directories.
        (path / 'bundle.tar.gz').unlink(missing_ok=True)
        atomic(path / 'cleanup.json', {'cleaned_at': now(), 'verified_local_sha256': expected,
                                      'removed': ['work', 'tmp', 'bundle.tar.gz']})
        return {'task_id': task, 'cleaned': True, 'retained': ['results', 'logs', 'manifest/result/status', 'task.sh', 'runner.py']}
    raise ValueError('Unknown operation')


def task_disk_bytes(root):
    total = 0
    def on_error(error):
        if not isinstance(error, FileNotFoundError):
            raise error
    for folder, directories, names in os.walk(root, followlinks=False, onerror=on_error):
        for name in names:
            try:
                info = (Path(folder) / name).lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
    return total


def run_task(task):
    path = job_path(task)
    manifest = json.loads((path / 'manifest.json').read_text())
    env = os.environ.copy()
    env.update(TASK_WORK=str(path / 'work'), TASK_TMP=str(path / 'tmp'),
               TASK_RESULTS=str(path / 'results'), TMPDIR=str(path / 'tmp'), PYTHONUNBUFFERED='1')
    start = now()
    log = path / 'logs' / 'combined.log'
    # A blocking logger thread owns the child's output; main thread can monitor disk usage.
    proc = subprocess.Popen(['/bin/bash', str(path / 'task.sh')], cwd=path / 'work', env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    counts = {'seen': 0, 'written': 0}
    def drain():
        with log.open('wb') as out:
            while True:
                data = proc.stdout.read1(65536)
                if not data:
                    break
                counts['seen'] += len(data)
                keep = data[:max(0, manifest['log_limit_bytes'] - counts['written'])]
                out.write(keep)
                out.flush()
                counts['written'] += len(keep)
    import threading
    thread = threading.Thread(target=drain, daemon=True)
    thread.start()
    disk_exceeded = False
    while proc.poll() is None:
        size = task_disk_bytes(path)
        if size > manifest['disk_limit_bytes']:
            disk_exceeded = True
            # Stop the full service cgroup, not only the shell.
            atomic(path / 'disk-limit.json', {'time': now(), 'bytes': size})
            subprocess.run(['kill', '-TERM', str(proc.pid)], capture_output=True)
            break
        time.sleep(2)
    if disk_exceeded:
        # Runner exits and KillMode=control-group removes any surviving descendants.
        atomic(path / 'result.json', {'started_at': start, 'ended_at': now(), 'exit_code': 125,
                                     'reason': 'disk_limit', 'logs_truncated': counts['seen'] > counts['written']})
        return 125
    rc = proc.wait()
    thread.join(timeout=10)
    atomic(path / 'result.json', {'started_at': start, 'ended_at': now(), 'exit_code': rc,
           'logs_bytes_seen': counts['seen'], 'logs_bytes_retained': counts['written'],
           'logs_truncated': counts['seen'] > counts['written']})
    return rc if rc >= 0 else 128 - rc


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--run':
        sys.exit(run_task(sys.argv[2]))
    payload = json.loads(sys.stdin.read())
    try:
        print(json.dumps(dispatch(payload['request'], payload['runner_source'])))
    except Exception as exc:
        print(json.dumps({'error': str(exc)}))
        sys.exit(1)
