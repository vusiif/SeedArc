# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixtures for check_solid_recipe.cmake (ADR-0006).

  big DIR                 three 7 MiB files (21 MiB > the 16 MiB recipe step), deterministic bytes
  retag IN OUT VALUE      copy IN to OUT with generation 0's revision-3 Seed value (or, for an
                          explicit record, the descriptor's second word) replaced by VALUE,
                          fixing the GEN record CRC, its trailer copy and the trailer CRC, so only
                          the recipe-vs-frames check can reject the result
  dedup DIR BIG           ADR-0008 tree from BIG's a b c: p/{a,a2,b,c,d,e0,e1} and q/a, where
                          a2, d and q/a copy a and e0, e1 are empty
  realias IN OUT PATH FRAME OFFSET
                          copy IN to OUT with PATH's trailer entry pointing at (FRAME, OFFSET),
                          size and CRC kept, fixing the trailer CRC (red_off archives only)
  far DIR                 ADR-0011 tree: r0 and r1 are 2 MiB of seeded pseudo-random bytes and r2
                          copies r0, so r2's only match lies 4 MiB back: beyond LZMA2 preset 0's
                          256 KiB dictionary, within a 128 MiB one
"""
from pathlib import Path
import struct
import sys

POLY = 0x82F63B78
TABLE = []
for n in range(256):
    c = n
    for _ in range(8):
        c = (c >> 1) ^ POLY if c & 1 else c >> 1
    TABLE.append(c)


def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = TABLE[(c ^ b) & 255] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def big(root):
    root = Path(root)
    root.mkdir(exist_ok=False)
    for i, name in enumerate(['a', 'b', 'c']):
        line = f'file {name}: the quick brown fox {i} jumps over the lazy dog\n'.encode()
        block = bytearray()
        n = 0
        while len(block) < 7 * 1024 * 1024:
            block += line + str(n).encode() + b'\n'
            n += 1
        (root / name).write_bytes(bytes(block[:7 * 1024 * 1024]))


def dedup(root, src):
    root, src = Path(root), Path(src)
    for d in ('p', 'q'):
        (root / d).mkdir(parents=True, exist_ok=False)
    a = (src / 'a').read_bytes()
    for name, data in [('a', a), ('a2', a), ('b', (src / 'b').read_bytes()), ('c', (src / 'c').read_bytes()),
                       ('d', a), ('e0', b''), ('e1', b'')]:
        (root / 'p' / name).write_bytes(data)
    (root / 'q' / 'a').write_bytes(a)


def realias(src, dst, path, frame, offset):
    data = bytearray(Path(src).read_bytes())
    toff, tsize = struct.unpack_from('<QI', data, len(data) - 24)
    trailer = bytearray(data[toff:toff + tsize])
    # Entry: path text (2-byte length, bytes), size (8), frame (4), offset (8), CRC (4). Find the path followed by a size
    # that fits, then rewrite frame and offset.
    key = struct.pack('<H', len(path.encode())) + path.encode()  # put_text: 2-byte length, bytes
    hits = []
    at = trailer.find(key)
    while at >= 0:
        hits.append(at + len(key))
        at = trailer.find(key, at + 1)
    hits = [h for h in hits if h + 24 <= len(trailer)]
    if len(hits) != 1:
        raise SystemExit('entry not unique')
    struct.pack_into('<IQ', trailer, hits[0] + 8, frame, offset)
    data[toff:toff + tsize] = trailer
    struct.pack_into('<I', data, len(data) - 12, crc32c(trailer))
    Path(dst).write_bytes(bytes(data))


def retag(src, dst, value):
    data = bytearray(Path(src).read_bytes())
    gen_at = 32  # generation 0's record directly follows the 32-byte header
    if data[gen_at:gen_at + 8] != b'SADXGEN4':
        raise SystemExit('unexpected layout')
    body_len = struct.unpack_from('<I', data, gen_at + 8)[0]
    body_at = gen_at + 12
    old_body = bytes(data[body_at:body_at + body_len])
    rev, length = struct.unpack_from('<HH', old_body, 16)
    if (rev, length) == (3, 4):
        at = 20  # the Seed value
    elif (rev, length) == (0, 8):
        at = 24  # explicit descriptor: second word (frame code, codec, extreme)
    else:
        raise SystemExit('generation 0 is not a revision-3 recipe record')
    body = bytearray(old_body)
    struct.pack_into('<I', body, at, value)
    data[body_at:body_at + body_len] = body
    struct.pack_into('<I', data, body_at + body_len, crc32c(body))
    toff, tsize = struct.unpack_from('<QI', data, len(data) - 24)
    trailer = bytes(data[toff:toff + tsize])
    if trailer.count(old_body) != 1:
        raise SystemExit('trailer copy not unique')
    trailer = trailer.replace(old_body, bytes(body))
    data[toff:toff + tsize] = trailer
    struct.pack_into('<I', data, len(data) - 12, crc32c(trailer))
    Path(dst).write_bytes(bytes(data))


def far(root):
    import random
    root = Path(root)
    root.mkdir(exist_ok=False)
    rng = random.Random(20261128)  # Mersenne Twister: the same bytes on every Python 3.9+
    r0 = rng.randbytes(2 * 1024 * 1024)
    (root / 'r0').write_bytes(r0)
    (root / 'r1').write_bytes(rng.randbytes(2 * 1024 * 1024))
    (root / 'r2').write_bytes(r0)


if __name__ == '__main__':
    if sys.argv[1] == 'big':
        big(sys.argv[2])
    elif sys.argv[1] == 'dedup':
        dedup(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'realias':
        realias(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), int(sys.argv[6]))
    elif sys.argv[1] == 'far':
        far(sys.argv[2])
    elif sys.argv[1] == 'retag':
        retag(sys.argv[2], sys.argv[3], int(sys.argv[4], 0))
    else:
        raise SystemExit('usage')
