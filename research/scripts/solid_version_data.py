# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate and apply only regular-file updates in a fixed Linux dev projection."""
import hashlib
import lzma
from pathlib import Path, PurePosixPath
import re
import subprocess
from append_history_r2 import sha, write_json

PATCH_SHA = 'ff1db6fd2cf36772bc9136f96062ec6b3fff7d6858a512b6a446731dc242f10d'


def safe_path(text):
    p = PurePosixPath(text)
    if (not text or p.is_absolute() or p.as_posix()!=text or '..' in p.parts
        or '\\' in text or ':' in text or any(ord(c)<32 for c in text)):
        raise ValueError('unsafe patch path')
    return text


def blob_id(data):
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()


def filter_patch(data, selected):
    if len(data)>32*1024**2 or not data.startswith(b'diff --git '):
        raise ValueError('unexpected patch envelope')
    blocks=re.split(rb'(?m)(?=^diff --git )',data)
    accepted, records, skipped=[], [], []
    for block in blocks:
        if not block:
            continue
        header=block.split(b'@@ ',1)[0]
        name=re.match(rb'diff --git a/([^\n]+) b/([^\n]+)\n',header)
        if not name:
            raise ValueError('unsupported diff header')
        old,new=[safe_path(s.decode('utf-8')) for s in name.groups()]
        if old not in selected and new not in selected:
            skipped.append(dict(old=old,new=new,reason='outside fixed path projection'))
            continue
        old_file=re.findall(rb'(?m)^--- ([^\n]+)\n',header)
        new_file=re.findall(rb'(?m)^\+\+\+ ([^\n]+)\n',header)
        index=re.search(rb'(?m)^index ([a-f0-9]+)\.\.([a-f0-9]+) 100(?:644|755)\n',header)
        if (old!=new or old_file!=[b'a/'+old.encode()] or new_file!=[b'b/'+new.encode()]
            or not index or b'@@ ' not in block or b'rename ' in header
            or b'new file mode' in header or b'deleted file mode' in header):
            raise ValueError('selected change is not a supported regular-file modification: '+old)
        before,after=[x.decode() for x in index.groups()]
        if len(before)<12 or len(after)<12:
            raise ValueError('insufficient upstream blob fingerprint')
        if any(r['path']==old for r in records):
            raise ValueError('duplicate patch target')
        accepted.append(block)
        records.append(dict(path=old,old_blob_prefix=before,new_blob_prefix=after))
    if not records:
        raise ValueError('no actual changes in selected projection')
    return b''.join(accepted), records, skipped


def apply_version(patch, v0, v1, selected, results):
    if sha(patch)!=PATCH_SHA:
        raise ValueError('official patch digest mismatch')
    with lzma.open(patch,'rb') as stream:
        raw=stream.read(32*1024**2+1)
    linux={r['path'].removeprefix('linux/'):r for r in selected if r['path'].startswith('linux/')}
    filtered,changes,skipped=filter_patch(raw,set(linux))
    for r in changes:
        p=v0/'linux'/r['path']
        if p.is_symlink() or not blob_id(p.read_bytes()).startswith(r['old_blob_prefix']):
            raise ValueError('upstream base blob differs: '+r['path'])
    filtered_path=results/'selected-version.patch'
    filtered_path.write_bytes(filtered)
    proc=subprocess.run(['patch','--batch','--forward','--posix','--fuzz=0','--no-backup-if-mismatch',
                         '-p1','-i',str(filtered_path.resolve())],cwd=v1/'linux',capture_output=True,
                        text=True,encoding='utf-8',timeout=120)
    (results/'version-patch.stdout').write_text(proc.stdout)
    (results/'version-patch.stderr').write_text(proc.stderr)
    if proc.returncode or re.search(r'offset|fuzz|FAILED|Reversed',proc.stdout+proc.stderr,re.I):
        raise RuntimeError('strict version patch failed; preserve evidence')
    changed={r['path']:r for r in changes}
    manifest=[]
    for r in selected:
        rel=r['path']
        base=(v0/rel).read_bytes()
        p=v1/rel
        if p.is_symlink() or not p.is_file():
            raise ValueError('version projection lost a regular file')
        data=p.read_bytes()
        inner=rel.removeprefix('linux/') if rel.startswith('linux/') else None
        if inner in changed:
            c=changed[inner]
            if data==base or not blob_id(data).startswith(c['new_blob_prefix']):
                raise ValueError('upstream updated blob differs: '+rel)
            c.update(old_bytes=len(base),new_bytes=len(data),old_sha256=hashlib.sha256(base).hexdigest(),
                     new_sha256=hashlib.sha256(data).hexdigest())
        elif base!=data:
            raise ValueError('unselected content changed')
        manifest.append('\t'.join([rel,str(len(data)),hashlib.sha256(data).hexdigest(),r['group'],r['batch']])+'\n')
    (results/'v1.tsv').write_text(''.join(manifest),encoding='utf-8')
    write_json(results/'version-delta.json',dict(scope='Linux6.12 to6.12.5, fixed existing dev paths only',
        official_patch_sha256=sha(patch),selected_patch_sha256=sha(filtered_path),
        changed_files=len(changes),unchanged_files=len(selected)-len(changes),changes=changes,
        skipped=skipped,additions=0,deletions=0,renames=0,
        limitations='Fixed regular-file projection; not the complete release tree. No archive update protocol.'))
    return results/'v1.tsv'
