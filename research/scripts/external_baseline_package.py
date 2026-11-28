# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-040: pack the local dev trees for the India run of external_baseline.py.

  python -I research/scripts/external_baseline_package.py OUT_TAR

Writes an uncompressed tar (compress it with zstd before sending) and OUT_TAR.manifest.tsv
(path, size, SHA256 per file, paths as stored in the tar) for the task to verify. Contents:
  append-r1-dev/            research/local/append-r1/files/dev   (includes native-bin)
  dup-local-dev-r1/<pool>/  research/local/dup-local-dev-r1/files (cargo, rustup, npm)
User decision 2026-11-27: this experiment may send these trees to the user's India server; no
holdout or test data is included.
"""
import hashlib
from pathlib import Path
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
TREES = [('append-r1-dev', ROOT / 'research/local/append-r1/files/dev'),
         ('dup-local-dev-r1', ROOT / 'research/local/dup-local-dev-r1/files')]


def main(out):
    out = Path(out)
    if out.exists():
        raise SystemExit('refuse to overwrite ' + str(out))
    rows = []
    with tarfile.open(out, 'w', format=tarfile.PAX_FORMAT, encoding='utf-8') as tf:
        for prefix, root in TREES:
            files = sorted((p for p in root.rglob('*') if p.is_file() and not p.is_symlink()),
                           key=lambda p: p.relative_to(root).as_posix().encode('utf-8'))
            for p in files:
                name = prefix + '/' + p.relative_to(root).as_posix()
                data = p.read_bytes()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mtime, info.mode = 0, 0o644
                with open(p, 'rb') as f:
                    tf.addfile(info, f)
                rows.append(f'{name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}\n')
    Path(str(out) + '.manifest.tsv').write_text(''.join(rows), encoding='utf-8', newline='\n')
    print(f'{len(rows)} files')


if __name__ == '__main__':
    main(sys.argv[1])
