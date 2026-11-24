# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the pre-registered mixed dev manifest from existing public dev indices."""
import argparse
import json
from pathlib import Path
import subprocess
from append_history_r2 import sha
from solid_search_manifest import key
from solid_version_data import safe_path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--path-tool',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    sources=[Path('research/datasets/append-linux-large-dev-r2/dev-linux-128.tsv'),
             Path('research/datasets/append-r1/dev-wiki-text.tsv'),
             Path('research/datasets/append-r1/dev-silesia.tsv')]
    rows=[]
    for source in sources:
        for line in source.read_text(encoding='utf-8').splitlines():
            row=line.split('\t')
            if len(row)!=5:
                raise ValueError('unexpected source index')
            if row[0]=='silesia/samba':
                continue
            safe_path(row[0])
            rows.append(row)
    if len(rows)!=7993 or len({r[0] for r in rows})!=7993 or len({r[2] for r in rows})!=7993:
        raise ValueError('pre-registered file count or dedup changed')
    text=''.join('\t'.join(r)+'\n' for r in rows)
    args.output.mkdir(parents=True,exist_ok=True)
    manifest=args.output/'v0.tsv'
    if manifest.exists() and manifest.read_text(encoding='utf-8')!=text:
        raise ValueError('refuse to overwrite a different manifest')
    checked=subprocess.run([str(args.path_tool),'--manifest'],input=''.join(r[0]+'\n' for r in rows),
                           text=True,capture_output=True,check=True)
    if not checked.stdout.startswith('ok\t'):
        raise ValueError('production manifest check failed')
    manifest.write_text(text,encoding='utf-8')
    meta=dict(files=len(rows),bytes=sum(int(r[1]) for r in rows),path_check=checked.stdout.strip(),
        v0_sha256=sha(manifest),expected_modified_files=127,
        source_manifests={p.as_posix():sha(p) for p in sources},
        source_pools={p:dict(files=sum(r[0].startswith(p+'/') for r in rows),
            bytes=sum(int(r[1]) for r in rows if r[0].startswith(p+'/'))) for p in ['linux','enwik8','silesia']},
        keys={name:len({key(i,r[0],int(r[1])) for r in rows}) for i,name in enumerate(['dir1','dir2','ext','size'])},
        scope='existing dev only; Linux fixed-path real-version projection; no holdout',
        excluded=['silesia/samba: keep source upload below16MiB, before performance observation'])
    (args.output/'metadata.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(files=meta['files'],bytes=meta['bytes'],sha256=meta['v0_sha256'])))


if __name__=='__main__':
    main()
