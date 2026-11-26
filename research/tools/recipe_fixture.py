# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixtures for check_solid_recipe.cmake (ADR-0006).

  big DIR                 three 7 MiB files (21 MiB > the 16 MiB recipe step), deterministic bytes
  retag IN OUT VALUE      copy IN to OUT with generation 0's revision-3 Seed value replaced by VALUE,
                          fixing the GEN record CRC, its trailer copy and the trailer CRC, so only
                          the recipe-vs-frames check can reject the result
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


def retag(src, dst, value):
    data = bytearray(Path(src).read_bytes())
    gen_at = 32  # generation 0's record directly follows the 32-byte header
    if data[gen_at:gen_at + 8] != b'SADXGEN4':
        raise SystemExit('unexpected layout')
    body_len = struct.unpack_from('<I', data, gen_at + 8)[0]
    body_at = gen_at + 12
    old_body = bytes(data[body_at:body_at + body_len])
    rev, length = struct.unpack_from('<HH', old_body, 16)
    if rev != 3 or length != 4:
        raise SystemExit('generation 0 is not a revision-3 Seed record')
    body = bytearray(old_body)
    struct.pack_into('<I', body, 20, value)
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


if __name__ == '__main__':
    if sys.argv[1] == 'big':
        big(sys.argv[2])
    elif sys.argv[1] == 'retag':
        retag(sys.argv[2], sys.argv[3], int(sys.argv[4], 0))
    else:
        raise SystemExit('usage')
