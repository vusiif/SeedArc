# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a task-local disk monitor fix; never modify the installed skill.

The skill's per-task runner can race with deletion between is_file and lstat.
Count regular files with one lstat, ignoring only files/directories gone during
the walk. Hardlink names still count separately and other errors still fail.
"""
import ast
import os
from pathlib import Path
import stat

FUNCTION = '''def task_disk_bytes(root):
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


'''
OLD = "        size = sum(f.lstat().st_size for f in path.rglob('*') if not f.is_symlink() and f.is_file())"


def patch(source):
    if source.count(OLD)!=1 or 'def task_disk_bytes(' in source:
        raise ValueError('unexpected runner source; do not patch')
    if "if size > manifest['disk_limit_bytes']:" not in source or 'time.sleep(2)' not in source:
        raise ValueError('unexpected disk monitoring contract')
    result=source.replace('import os\n','import os\nimport stat\n',1)
    result=result.replace('def run_task(task):',FUNCTION+'def run_task(task):',1)
    result=result.replace(OLD,'        size = task_disk_bytes(path)',1)
    ast.parse(result)
    if FUNCTION not in result:
        raise ValueError('runner entry point not found')
    return result


def check(root):
    from unittest.mock import patch as mock_patch
    namespace=dict(os=os,Path=Path,stat=stat)
    exec(FUNCTION,namespace)
    count=namespace['task_disk_bytes']
    root.mkdir(exist_ok=False)
    first=root/'first'
    first.write_bytes(b'1234567')
    os.link(first,root/'second')
    assert count(root)==14,'hardlink names must each count'
    vanished=root/'vanished'
    vanished.write_bytes(b'x')
    original=Path.lstat

    def race(path,*args,**kwargs):
        if path==vanished:
            path.unlink()
        return original(path,*args,**kwargs)
    with mock_patch.object(Path,'lstat',race):
        assert count(root)==14

    def denied(path,*args,**kwargs):
        raise PermissionError('simulated task file access failure')
    with mock_patch.object(Path,'lstat',denied):
        try:
            count(root)
        except PermissionError:
            pass
        else:
            raise AssertionError('permission failure was suppressed')
    first.unlink()
    (root/'second').unlink()
    root.rmdir()
    return dict(hardlink_names_counted=True,vanished_file_race_handled=True,permission_errors_propagated=True)
