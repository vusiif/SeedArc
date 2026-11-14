# SPDX-License-Identifier: GPL-3.0-or-later
"""Build b2r5-corpus-r2: dev / test split by project group with content-hash disjointness.

Fixes the r1 defect (path-hash split let identical content land on both sides). Every file
belongs to a project group; groups that share any content SHA256 (also across pools) are merged
into one component; whole components go to dev or test by a fixed hash of the component key, so
no test file can have content that exists in dev. `check_disjoint()` re-verifies this from the
committed listings and is also called by the evaluation script before it measures anything.
Empty files are excluded (they would join every group). No download; content goes to the ignored
research/local/<corpus>/, metadata (relative paths, sizes, SHA256, groups) to research/datasets/.

  python research/scripts/b2_corpus_r2.py            # build (refuses an existing corpus)
  python research/scripts/b2_corpus_r2.py --check    # re-verify disjointness from listings
"""
import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import unicodedata
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
CORPUS = "b2r5-corpus-r2"
CHUNK = 65536
MiB = 1024 * 1024
CMAKE_SHARE = Path(shutil.which("cmake")).resolve().parents[1] / "share" if shutil.which("cmake") else None
PYLIB = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs/Python/Python315/Lib"


def top3(rel):
    return "/".join(rel.split("/")[:3])


# pool -> list of (source label, root, keep(rel), group(rel)); caps per split
POOLS = {
    "many-small-docs": {
        "type": "many small files (reStructuredText docs)",
        "license": "BSD-3-Clause (Kitware CMake documentation)",
        "redistribution": "local evaluation only; not redistributed",
        "cap_bytes": 4 * MiB, "cap_files": 2000,
        "sources": [("cmake-share", CMAKE_SHARE, lambda r: r.endswith(".rst"), top3)],
        "grouping": "CMake share/<version>/Help/<section>",
    },
    "source-python": {
        "type": "source code (Python standard library)",
        "license": "PSF-2.0 (CPython standard library)",
        "redistribution": "local evaluation only; not redistributed",
        "cap_bytes": 8 * MiB, "cap_files": 2000,
        "sources": [("python-lib", PYLIB,
                     lambda r: r.endswith(".py") and not r.startswith(("test/", "site-packages/"))
                     and "/test/" not in r and "/tests/" not in r and "/idle_test/" not in r,
                     lambda r: r.split("/")[0] if "/" in r else r)],
        "grouping": "top-level package (a top-level module file is its own group)",
    },
    "config-cmake-pkg": {
        "type": "configuration (CMake modules, package configs, pkg-config, MSYS2 etc)",
        "license": "BSD-3-Clause (CMake modules); per package (MSYS2 configs)",
        "redistribution": "local evaluation only; not redistributed",
        "cap_bytes": 8 * MiB, "cap_files": 2000,
        "sources": [("cmake-share", CMAKE_SHARE, lambda r: r.endswith((".cmake", ".in")), top3),
                    ("ucrt64-lib-cmake", Path(r"C:\msys64\ucrt64\lib\cmake"), lambda r: True, lambda r: r.split("/")[0]),
                    ("ucrt64-pkgconfig", Path(r"C:\msys64\ucrt64\lib\pkgconfig"), lambda r: r.endswith(".pc"), lambda r: r),
                    ("msys2-etc", Path(r"C:\msys64\etc"), lambda r: True, lambda r: r.split("/")[0])],
        "grouping": "CMake share/<version>/Modules/<dir> | package | .pc file | etc/<top>",
    },
}


RESERVED = {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$",
            *(f"{p}{d}" for p in ("COM", "LPT") for d in "123456789¹²³")}


def archive_path_ok(rel):
    """Mirror of the production Windows path profile (core/filesystem/archive_path.cpp), so every
    corpus file can also be stored by the production reference archives."""
    if not rel or len(rel.encode("utf-8")) > 4096 or not unicodedata.is_normalized("NFC", rel):
        return False
    if any(ord(c) < 32 or ord(c) == 127 or c in '\\:<>"|?*~' for c in rel):
        return False
    parts = rel.split("/")
    if len(parts) > 64:
        return False
    for part in parts:
        base = part.split(".")[0].rstrip(" ").upper()
        if not part or part in (".", "..") or part[-1] in ". " or len(part) > 255 or base in RESERVED:
            return False
    return True


def digest(data):
    return hashlib.sha256(data).hexdigest()


def scan():
    files = []  # (pool, rel_in_corpus, size, sha, node, src_path)
    skipped = defaultdict(int)
    rejected = []
    for pool, spec in POOLS.items():
        for label, root, keep, group in spec["sources"]:
            if root is None or not root.is_dir():
                skipped[f"{pool}:{label}:missing"] += 1
                continue
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames.sort()
                for name in sorted(filenames):
                    p = Path(dirpath) / name
                    rel = p.relative_to(root).as_posix()
                    if p.is_symlink() or not keep(rel):
                        continue
                    if not archive_path_ok(f"{label}/{rel}"):
                        skipped[f"{pool}:path_rejected_by_archive"] += 1
                        rejected.append(f"{label}/{rel}")
                        continue
                    try:
                        data = p.read_bytes()
                    except OSError:
                        skipped[f"{pool}:unreadable"] += 1
                        continue
                    if not data:
                        skipped[f"{pool}:empty"] += 1
                        continue
                    files.append((pool, f"{label}/{rel}", len(data), digest(data), f"{pool}|{label}|{group(rel)}", p))
    return files, dict(skipped), sorted(rejected)


def components(files):
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    first = {}
    for _, _, _, sha, node, _ in files:
        find(node)
        if sha in first:
            a, b = find(node), find(first[sha])
            if a != b:
                parent[max(a, b)] = min(a, b)
        else:
            first[sha] = node
    members = defaultdict(set)
    for node in parent:
        members[find(node)].add(node)
    return {node: min(members[find(node)]) for node in parent}


def split_of(component_key):
    return "dev" if hashlib.sha256(f"{CORPUS}:{component_key}".encode()).digest()[0] % 2 == 0 else "test"


def read_listing(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        rel, size, sha, component = line.split("\t")
        rows.append((rel, int(size), sha, component))
    return rows


def check_disjoint(corpus=CORPUS):
    """Raise if any test file's content SHA256 also occurs in dev, or a component spans both."""
    meta = ROOT / "research/datasets" / corpus
    seen = {"dev": {}, "test": {}}
    comp_side = {}
    for listing in sorted(meta.glob("*.tsv")):
        split = listing.name.split("-", 1)[0]
        for rel, size, sha, component in read_listing(listing):
            seen[split][sha] = rel
            if comp_side.setdefault(component, split) != split:
                raise AssertionError(f"component {component} on both sides")
    shared = set(seen["dev"]) & set(seen["test"])
    if shared:
        raise AssertionError(f"{len(shared)} content hashes in both dev and test, e.g. {seen['test'][next(iter(shared))]}")
    return {"dev_hashes": len(seen["dev"]), "test_hashes": len(seen["test"]), "shared": 0, "components": len(comp_side)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        print(json.dumps(check_disjoint()))
        return
    local, meta = ROOT / "research/local" / CORPUS, ROOT / "research/datasets" / CORPUS
    if local.exists() or meta.exists():
        parser.error("corpus exists; use a new corpus ID")
    files, skipped, rejected = scan()
    comp = components(files)
    meta.mkdir(parents=True)
    manifest = {"corpus_id": CORPUS, "created_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
                "source": "data already installed on the evaluation machine; no download",
                "machine": {"os": platform.platform(),
                            "cmake": subprocess.run(["cmake", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
                            "python_lib": f"CPython {sys.version.split()[0]} standard library"},
                "split_policy": ("project groups merged into components by shared content SHA256 (across pools); "
                                 f"component -> sha256('{CORPUS}:<component key>')[0] even: dev, odd: test; "
                                 "fixed before any compression; test never used for tuning; empty files excluded"),
                "selection": "per pool and split: sorted corpus paths until the byte / file caps (whole components never move sides)",
                "chunk_bytes": CHUNK, "skipped": skipped, "path_rejected_by_archive": rejected,
                "revision": 2, "revision_note": ("2026-11-13: rebuilt with the production archive path filter after "
                    "the first dev sweep failed on 'msys2-etc/pacman.d/gnupg/pubring.gpg~'; no test-split measurement "
                    "had been made; revision 1 listings remain in git history (commit 9284eae)"), "fixes": "b2r5-corpus-r1 known defect r1-dev-test-content-overlap",
                "datasets": {}}
    sizes = defaultdict(int)
    for pool, rel, size, sha, node, _ in files:
        sizes[comp[node]] += size
    for pool, spec in POOLS.items():
        rows = {"dev": [], "test": []}
        totals = {"dev": [0, 0], "test": [0, 0]}
        for p, rel, size, sha, node, src in sorted(x for x in files if x[0] == pool):
            side = split_of(comp[node])
            n, b = totals[side]
            if n >= spec["cap_files"] or b + size > spec["cap_bytes"]:
                continue
            dest = local / side / pool / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(src.read_bytes())
            rows[side].append((rel, size, sha, comp[node]))
            totals[side] = [n + 1, b + size]
        entry = {"type": spec["type"], "license": spec["license"], "redistribution": spec["redistribution"],
                 "grouping": spec["grouping"], "sources": [s[0] for s in spec["sources"]],
                 "caps": {"split_bytes": spec["cap_bytes"], "split_files": spec["cap_files"]}, "splits": {}}
        for side, rs in rows.items():
            rs.sort()
            text = "".join(f"{r}\t{s}\t{h}\t{c}\n" for r, s, h, c in rs)
            (meta / f"{side}-{pool}.tsv").write_text(text, encoding="utf-8", newline="\n")
            entry["splits"][side] = {"dataset_id": f"{side}/{pool}", "file_count": len(rs), "bytes": sum(s for _, s, _, _ in rs),
                                     "blocks": sum((s + CHUNK - 1) // CHUNK for _, s, _, _ in rs),
                                     "components": len({c for *_, c in rs}), "distinct_content": len({h for _, _, h, _ in rs}),
                                     "median_file_bytes": sorted(s for _, s, _, _ in rs)[len(rs) // 2] if rs else None,
                                     "listing": f"{CORPUS}/{side}-{pool}.tsv", "listing_sha256": digest(text.encode())}
        manifest["datasets"][pool] = entry
        print(pool, {k: (v["file_count"], v["bytes"], v["components"]) for k, v in entry["splits"].items()}, flush=True)
    manifest["largest_components"] = sorted(([k, v] for k, v in sizes.items()), key=lambda x: -x[1])[:5]
    manifest["disjointness_check"] = check_disjoint()
    (ROOT / "research/datasets" / f"{CORPUS}.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                                               encoding="utf-8")
    print("disjoint:", manifest["disjointness_check"], "largest:", manifest["largest_components"][:3])


if __name__ == "__main__":
    main()
