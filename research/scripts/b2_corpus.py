# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the B2-r5 local corpus (dev / independent test) from data already on this machine.

No download. Every pool is split into dev / test by a fixed hash of its relative path BEFORE any
compression is run; selection inside a split is by sorted path under fixed caps. Content is
copied into the ignored research/local/<corpus>/; only metadata (relative paths, sizes, SHA256,
licences) goes into research/datasets/. Absolute source roots are not written to the manifest
(they can contain the user name); each pool records a stable, redacted source description.

  python research/scripts/b2_corpus.py [--corpus-id b2r5-corpus-r1]
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
CHUNK = 65536
MiB = 1024 * 1024


def cmake_share():
    exe = shutil.which("cmake")
    return Path(exe).resolve().parents[1] / "share" if exe else None


VC = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC")
# id, type, source root, redacted source text, licence, redistribution, extensions (None: all),
# split cap (bytes), split cap (files), per-file cap (bytes)
POOLS = [
    ("source-headers", "source / text", Path(r"C:\msys64\ucrt64\include"),
     "MSYS2 ucrt64/include (installed package headers)",
     "per package; mingw-w64 headers ZPL-2.1 / public domain, others see ucrt64/share/licenses",
     "local evaluation only; not redistributed", None, 12 * MiB, 2000, 4 * MiB),
    ("config-cmake", "config / text", cmake_share(),
     "CMake installation share/ (modules, templates, help)",
     "BSD-3-Clause (Kitware CMake)", "local evaluation only; not redistributed",
     None, 12 * MiB, 2000, 4 * MiB),
    ("logs-windows", "logs", Path(r"C:\Windows\Logs"),
     "Windows\\Logs text logs of this machine",
     "none (machine-generated; may contain machine identifiers)",
     "local only; never committed or shared", {".log", ".txt"}, 12 * MiB, 2000, 4 * MiB),
    ("structured-inf", "structured data", Path(r"C:\Windows\INF"),
     "Windows\\INF driver setup files (INI-like structured text)",
     "Microsoft / hardware vendors (system files)", "local only; not redistributable",
     {".inf"}, 12 * MiB, 2000, 4 * MiB),
    ("binary-native", "native binary", Path(r"C:\msys64\ucrt64\bin"),
     "MSYS2 ucrt64/bin DLL and EXE", "per package (GPL / LGPL / MIT / BSD ...); see ucrt64/share/licenses",
     "local evaluation only; not redistributed", {".dll", ".exe"}, 16 * MiB, 2000, 4 * MiB),
    ("compressed-packages", "already compressed (zstd archives)", Path(r"C:\msys64\var\cache\pacman\pkg"),
     "MSYS2 pacman package cache *.pkg.tar.zst", "per package; see each package",
     "local evaluation only; not redistributed", {".zst"}, 16 * MiB, 2000, 4 * MiB),
    ("image-jpeg", "already compressed (images)", Path(r"C:\Windows\Web"),
     "Windows\\Web wallpapers and screen images", "Microsoft (Windows component)",
     "local only; not redistributable", {".jpg", ".jpeg", ".png"}, 12 * MiB, 2000, 8 * MiB),
    ("audio-pcm", "audio (uncompressed PCM WAV)", Path(r"C:\Windows\Media"),
     "Windows\\Media system sounds (WAV PCM)", "Microsoft (Windows component)",
     "local only; not redistributable", {".wav"}, 12 * MiB, 2000, 4 * MiB),
    ("many-small-terminfo", "many small files", Path(r"C:\msys64\usr\share\terminfo"),
     "MSYS2 ncurses terminfo database (compiled entries)", "ncurses: MIT-X11 style",
     "local evaluation only; not redistributed", None, 12 * MiB, 2000, 4 * MiB),
    ("duplicate-licenses", "cross-file duplicates", Path(r"C:\msys64"),
     "MSYS2 usr/share/licenses + ucrt64/share/licenses (many identical licence texts)",
     "licence texts themselves (GPL / LGPL / MIT / ...)", "local evaluation only",
     None, 12 * MiB, 2000, 4 * MiB),
]
DUP_SUBDIRS = [Path("usr/share/licenses"), Path("ucrt64/share/licenses")]


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def split_of(corpus, pool, rel):
    h = hashlib.sha256(f"{corpus}:{pool}:{rel}".encode("utf-8")).digest()
    return "dev" if h[0] % 2 == 0 else "test"


def candidates(pool_id, root, exts):
    roots = [root / s for s in DUP_SUBDIRS] if pool_id == "duplicate-licenses" else [root]
    out = []
    for r in roots:
        if not r.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(r):
            dirnames.sort()
            for name in sorted(filenames):
                p = Path(dirpath) / name
                if p.is_symlink() or (exts is not None and p.suffix.lower() not in exts):
                    continue
                rel = p.relative_to(root).as_posix()
                out.append((rel, p))
    return sorted(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus-id", default="b2r5-corpus-r1")
    args = parser.parse_args()
    corpus = args.corpus_id
    local = ROOT / "research/local" / corpus
    meta_dir = ROOT / "research/datasets" / corpus
    if local.exists() or meta_dir.exists():
        parser.error("corpus exists; use a new corpus ID")
    meta_dir.mkdir(parents=True)
    cmake_version = subprocess.run(["cmake", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    manifest = {"corpus_id": corpus, "created_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
                "source": "data already installed on the evaluation machine; no download",
                "machine": {"os": platform.platform(), "cmake": cmake_version},
                "split_policy": "sha256('<corpus>:<pool>:<relative_path>')[0] even -> dev, odd -> test; "
                                "fixed before any compression; test is never used to tune parameters",
                "selection": "per split: sorted relative paths; skip files over the per-file cap, symlinks, "
                             "unreadable files and paths the archive rejects (recorded); stop at the byte / file caps",
                "chunk_bytes": CHUNK, "datasets": {}}
    for pool_id, kind, root, source_text, licence, redistribution, exts, cap_bytes, cap_files, file_cap in POOLS:
        if root is None or not root.is_dir():
            manifest["datasets"][pool_id] = {"status": "source missing", "source": source_text}
            continue
        per_split = {"dev": [], "test": []}
        skipped = {"over_file_cap": 0, "unreadable": 0, "empty_name_or_reserved": 0}
        totals = {"dev": [0, 0], "test": [0, 0]}
        for rel, path in candidates(pool_id, root, exts):
            split = split_of(corpus, pool_id, rel)
            files, size = totals[split]
            if files >= cap_files or size >= cap_bytes:
                continue
            try:
                st = path.stat()
                if st.st_size > file_cap:
                    skipped["over_file_cap"] += 1
                    continue
                if size + st.st_size > cap_bytes:
                    continue
                data = path.read_bytes()
            except OSError:
                skipped["unreadable"] += 1
                continue
            if any(part.rstrip(". ") != part or part.split(".")[0].upper() in
                   {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(10)), *(f"LPT{i}" for i in range(10))}
                   for part in rel.split("/")):
                skipped["empty_name_or_reserved"] += 1
                continue
            dest = local / split / pool_id / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            per_split[split].append((rel, len(data), digest_bytes(data)))
            totals[split] = [files + 1, size + len(data)]
        entry = {"type": kind, "source": source_text, "license": licence, "redistribution": redistribution,
                 "extensions": sorted(exts) if exts else "all", "caps": {"split_bytes": cap_bytes,
                 "split_files": cap_files, "per_file_bytes": file_cap}, "skipped": skipped, "splits": {}}
        for split, rows in per_split.items():
            rows.sort()
            listing = "".join(f"{r}\t{s}\t{h}\n" for r, s, h in rows)
            (meta_dir / f"{split}-{pool_id}.tsv").write_text(listing, encoding="utf-8", newline="\n")
            entry["splits"][split] = {
                "dataset_id": f"{split}/{pool_id}", "file_count": len(rows),
                "bytes": sum(s for _, s, _ in rows), "blocks": sum((s + CHUNK - 1) // CHUNK for _, s, _ in rows),
                "empty_files": sum(1 for _, s, _ in rows if s == 0),
                "listing": f"{corpus}/{split}-{pool_id}.tsv", "listing_sha256": digest_bytes(listing.encode("utf-8")),
                "distinct_content": len({h for _, _, h in rows})}
        manifest["datasets"][pool_id] = entry
        print(pool_id, {k: (v["file_count"], v["bytes"]) for k, v in entry["splits"].items()}, skipped, flush=True)
    (ROOT / "research/datasets" / f"{corpus}.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
