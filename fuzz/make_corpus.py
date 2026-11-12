"""Independent (Python) encoder for the experimental SeedArc container, used to create fuzz seeds.

It follows ADR-0001 / ADR-0002 directly and shares no code with the C++ implementation, so the
C++ reader accepting these files is also a cross-check of the documented wire format.
Usage: make_corpus.py <output directory>
"""
import struct
import sys
from pathlib import Path


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def index_records(entries, blocks) -> bytes:
    out = b''
    for path, directory, size, first, count in entries:
        raw = path.encode('utf-8')
        out += struct.pack('<IB3xQQQ', len(raw), 1 if directory else 0, size, first, count) + raw
    for offset, stored, raw_size, checksum in blocks:
        out += struct.pack('<QQQIII12x', offset, stored, raw_size, 0, 0, checksum)
    return out


def archive(files, chunk=1024 * 1024) -> bytes:
    """files: list of (path, None for a directory or bytes), already in canonical order."""
    entries, blocks, data = [], [], b''
    for path, content in files:
        if content is None:
            entries.append((path, True, 0, len(blocks), 0))
            continue
        first = len(blocks)
        for start in range(0, len(content), chunk):
            piece = content[start:start + chunk]
            blocks.append((len(data), len(piece), len(piece), crc32c(piece)))
            data += piece
        entries.append((path, False, len(content), first, len(blocks) - first))
    index = b''
    if entries:
        records = index_records(entries, blocks)
        index = records + struct.pack('<I', crc32c(records))
    header = b'SARCEXP\0' + struct.pack('<HHIIHHQQQQQ16sIII', 1, 96, 0, 0, 0, 1, len(index),
                                        len(entries), len(blocks), len(data), len(data),
                                        bytes(16), chunk, 0, 0)
    header += struct.pack('<I', crc32c(header))
    assert len(header) == 96
    return header + data + index


def main() -> None:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    tree = [('a', None), ('a/b.bin', b'0123456789'), ('a/empty', b''), ('c.txt', b'text'),
            ('d', None), ('中文.txt', '内容'.encode('utf-8'))]
    (out / 'empty-archive.bin').write_bytes(archive([]))
    (out / 'one-file.bin').write_bytes(archive([('file.txt', b'hello')]))
    (out / 'tree-chunk4.bin').write_bytes(archive(tree, chunk=4))
    (out / 'truncated.bin').write_bytes(archive(tree, chunk=4)[:-7])
    # Seed for the repaired-index path: 16 parameter bytes (entry count, block count, total
    # output, chunk size - 1 as little-endian u16, rest zero) followed by the index records.
    entries = [('a', True, 0, 0, 0), ('a/b', False, 6, 0, 2), ('c', False, 0, 2, 0)]
    blocks = [(0, 4, 4, 1), (4, 2, 2, 2)]
    (out / 'repaired-index.bin').write_bytes(
        struct.pack('<HHHH8x', len(entries), len(blocks), 6, 3) + index_records(entries, blocks))


if __name__ == '__main__':
    main()
