# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-041: how large the research container's trailer is and how far it compresses.

  python -I research/scripts/trailer_probe.py --tool SEEDARC_APPEND4_STUDY --work EMPTY_DIR NAME=DIR ...

For every tree: recipe-pack with solid/zstd-3/g1-r0/frame-32MiB (the trailer does not depend on
the codec or level; only the frame count can differ slightly between recipes), read the trailer
through the footer (offset 8 bytes and size 4 bytes, 24 bytes before the end), and compress it
with LZMA preset 9e and, when the interpreter has it, Zstandard 19. Reported per tree: files,
archive and trailer bytes, trailer share, compressed sizes and the archive size if the trailer
were stored compressed (plus a 16-byte record header estimate). Measurement only; no format.
"""
import argparse
import json
import lzma
from pathlib import Path
import struct
import subprocess
import sys

try:
    from compression import zstd
except ImportError:  # Python < 3.14
    zstd = None


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--tool', required=True)
    a.add_argument('--work', required=True)
    a.add_argument('trees', nargs='+')
    a = a.parse_args()
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=False)
    rows = []
    for spec in a.trees:
        name, path = spec.split('=', 1)
        arc = work / (name + '.sadx')
        info = json.loads(subprocess.run([a.tool, 'recipe-pack', 'seed', 'solid/zstd-3/g1-r0/frame-32MiB', path,
                                          str(arc)], capture_output=True, check=True).stdout)
        data = arc.read_bytes()
        off, size = struct.unpack_from('<QI', data, len(data) - 24)
        trailer = data[off:off + size]
        if size != info['parts']['trailer']:
            raise SystemExit('trailer size disagrees with the tool')
        lz = len(lzma.compress(trailer, preset=9 | lzma.PRESET_EXTREME))
        zs = len(zstd.compress(trailer, level=19)) if zstd else None
        best = min(x for x in (lz, zs) if x is not None)
        row = dict(tree=name, files=info['files'], archive_bytes=len(data), trailer_bytes=size,
                   trailer_share=round(size / len(data), 4), trailer_lzma9e=lz, trailer_zstd19=zs,
                   bytes_per_file=round(size / max(info['files'], 1), 1),
                   archive_if_compressed=len(data) - size + best + 16,
                   saving=round((size - best - 16) / len(data), 4))
        rows.append(row)
        print(json.dumps(row), flush=True)
        arc.unlink()
    work.rmdir()


if __name__ == '__main__':
    main()
