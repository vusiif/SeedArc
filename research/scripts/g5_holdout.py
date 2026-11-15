# SPDX-License-Identifier: GPL-3.0-or-later
"""Build g5-holdout-r1: the one-time evaluation set for G5 (DEV-016, research/ideas/seed-g5-r1.md).

Three pools mirror b2r5-corpus-r2 (docs / Python source / CMake-package config). Each pool has a
"local" half (data installed on this machine; listings only, never redistributed) and a "public"
half (pinned public releases: Django 6.1.3 sdist, vcpkg ports at a fixed commit). Rules:

  * a file whose content SHA256 occurs anywhere in b2r5-corpus-r2 (dev or test) is excluded;
  * every corpus path must pass the production path profile (ADR-0005), checked by the
    seedarc_path_check tool built from libseedarc, not by a Python approximation;
  * empty files and symbolic links are excluded;
  * whole project groups are taken in a fixed hash order until the half's byte / file cap.

Content is copied to the ignored research/local/g5-holdout-r1/files/; listings (relative path,
size, SHA256, group) and the manifest go to research/datasets/. Refuses to overwrite.

  python -I research/scripts/g5_holdout.py --path-check <seedarc_path_check> --django <dir> \
      --wsl-root '\\\\wsl.localhost\\Ubuntu-26.04'
  python -I research/scripts/g5_holdout.py --check     # re-verify r2 exclusion from listings
"""
import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import stat
import subprocess
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
CORPUS = "g5-holdout-r1"
R2 = ROOT / "research/datasets/b2r5-corpus-r2"
CHUNK = 65536
MiB = 1024 * 1024
DJANGO_SDIST = {"file": "django-6.1.3.tar.gz", "bytes": 11306257,
                "sha256": "b5d2d9e35295065d13b1927b3f19a59bd119f737aa6299def14ccca1ae3c8c0d",
                "source": "https://files.pythonhosted.org (PyPI), downloaded 2026-11-14",
                "license": "BSD-3-Clause"}
VCPKG_COMMIT = "fe8e78fa"


def first(rel):
    return rel.split("/")[0] if "/" in rel else rel


def first2(rel):
    return "/".join(rel.split("/")[:2]) if rel.count("/") >= 2 else first(rel)


def not_tests(rel):
    parts = rel.split("/")
    return not any(p in ("test", "tests", "testing") for p in parts[:-1])


def sources(args):
    py = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs/Python/Python315/Lib/site-packages"
    wsl = Path(args.wsl_root)
    django = Path(args.django)
    text_docs = (".txt", ".rst", ".md", ".html", ".htm", ".texi", ".info")
    return {
        "many-small-docs": {
            "type": "many small files (documentation text, at most 32 KiB each)",
            "caps": {"local": (2 * MiB, 1000), "public": (2 * MiB, 1000)},
            "max_file_bytes": 32 * 1024,
            "halves": {
                "local": [("msys2-ucrt64-doc", Path(r"C:\msys64\ucrt64\share\doc"),
                           lambda r: r.lower().endswith(text_docs), first,
                           "per package (MSYS2 UCRT64 documentation); local evaluation only")],
                "public": [("django-docs", django / "docs", lambda r: r.endswith(".txt"), first,
                            "BSD-3-Clause (Django 6.1.3 documentation)")],
            },
        },
        "source-python": {
            "type": "source code (Python, third-party, not the standard library)",
            "caps": {"local": (3 * MiB, 1000), "public": (3 * MiB, 1000)},
            "halves": {
                "local": [("py315-site-packages", py,
                           lambda r: r.endswith(".py") and not_tests(r) and ".dist-info/" not in r,
                           first, "per package (third-party site-packages); local evaluation only")],
                "public": [("django-src", django / "django",
                            lambda r: r.endswith(".py") and not_tests(r), first2,
                            "BSD-3-Clause (Django 6.1.3)")],
            },
        },
        "config-cmake-pkg": {
            "type": "configuration (MSBuild props / targets, CMake package configs, pkg-config)",
            "caps": {"local": (2 * MiB, 1000), "public": (2 * MiB, 1000)},
            "halves": {
                "local": [
                    ("msbuild-vc-v180", Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
                                             r"\MSBuild\Microsoft\VC\v180"),
                     lambda r: r.lower().endswith((".props", ".targets", ".xml")), first,
                     "Microsoft (Visual Studio Build Tools); local evaluation only"),
                    ("wsl-lib-cmake", wsl / "usr/lib/x86_64-linux-gnu/cmake", lambda r: True, first,
                     "per Ubuntu package; local evaluation only"),
                    ("wsl-lib-pkgconfig", wsl / "usr/lib/x86_64-linux-gnu/pkgconfig",
                     lambda r: r.endswith(".pc"), lambda r: r, "per Ubuntu package; local evaluation only"),
                    ("wsl-share-pkgconfig", wsl / "usr/share/pkgconfig",
                     lambda r: r.endswith(".pc"), lambda r: r, "per Ubuntu package; local evaluation only"),
                ],
                "public": [("vcpkg-ports", wsl / "home" / args.wsl_user / "vcpkg/ports",
                            lambda r: r.endswith((".cmake", ".json", ".cmake.in")) or r.endswith("/usage"),
                            first, f"MIT (microsoft/vcpkg at {VCPKG_COMMIT})")],
            },
        },
    }


def digest(data):
    return hashlib.sha256(data).hexdigest()


def r2_hashes():
    hashes = set()
    for listing in sorted(R2.glob("*.tsv")):
        for line in listing.read_text(encoding="utf-8").splitlines():
            hashes.add(line.split("\t")[2])
    return hashes


def path_verdicts(tool, paths):
    data = "".join(p + "\n" for p in paths).encode("utf-8")
    out = subprocess.run([tool], input=data, capture_output=True, check=True).stdout.decode("utf-8")
    verdicts = {}
    for line in out.splitlines():
        line = line.rstrip("\r")
        code, rest = line.split("\t", 1)
        verdicts[rest.split("\t", 1)[0]] = code
    if len(verdicts) != len(set(paths)):
        raise RuntimeError("path check tool returned a different number of verdicts")
    return verdicts


def wsl_regular_files(args, root):
    """Relative paths of regular files under a WSL directory, listed inside WSL itself: through
    \\\\wsl.localhost a symbolic link is not reliably reported as one, so lstat cannot be trusted."""
    linux = "/" + root.relative_to(Path(args.wsl_root)).as_posix()
    # -print0 needs no escape sequence (a "\0" in -printf does not survive wsl.exe argument passing).
    out = subprocess.run(["wsl.exe", "-d", args.wsl_distro, "--", "find", linux, "-type", "f", "-print0"],
                         capture_output=True, check=True).stdout
    prefix = (linux.rstrip("/") + "/").encode("utf-8")
    names = [p for p in out.split(b"\0") if p]
    if not names or not all(p.startswith(prefix) for p in names):
        raise RuntimeError(f"unexpected find output for {linux}")
    return {p[len(prefix):].decode("utf-8") for p in names}


def walk(args, root):
    """Regular files under root, sorted, without following or including symbolic links."""
    allowed = None
    if str(root).lower().startswith(str(Path(args.wsl_root)).lower()):
        allowed = wsl_regular_files(args, root)
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = sorted(d for d in dirnames if not os.path.islink(os.path.join(dirpath, d)))
        for name in sorted(filenames):
            p = Path(dirpath) / name
            if allowed is not None:
                if p.relative_to(root).as_posix() in allowed:
                    yield p
                continue
            if stat.S_ISREG(os.lstat(p).st_mode):
                yield p


def build(args):
    local_root, meta = ROOT / "research/local" / CORPUS / "files", ROOT / "research/datasets" / CORPUS
    if local_root.exists() or meta.exists() or (ROOT / f"research/datasets/{CORPUS}.json").exists():
        raise SystemExit("corpus exists; use a new corpus ID")
    excluded_hashes = r2_hashes()
    spec = sources(args)
    manifest = {"corpus_id": CORPUS, "created_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
                "purpose": "one-time G5 evaluation (DEV-016); never used for tuning",
                "plan": "research/ideas/seed-g5-r1.md",
                "machine": {"os": platform.platform()},
                "exclusion": {"r2_content_hashes": len(excluded_hashes),
                              "rule": "content SHA256 present in any b2r5-corpus-r2 listing (dev or test)"},
                "path_profile": "ADR-0005 via seedarc_path_check (libseedarc validate_archive_path)",
                "selection": ("per pool and half: project groups in sha256('g5-holdout-r1:<group>') order, "
                              "whole groups only, until the byte / file cap; empty files and symlinks excluded"),
                "public_inputs": {"django_sdist": DJANGO_SDIST, "vcpkg": {"repository": "microsoft/vcpkg",
                                  "commit": VCPKG_COMMIT, "path": "ports/", "license": "MIT"}},
                "chunk_bytes": CHUNK, "datasets": {}, "skipped": {}, "path_rejected": []}
    skipped = defaultdict(int)
    for pool, pool_spec in spec.items():
        entry = {"type": pool_spec["type"], "max_file_bytes": pool_spec.get("max_file_bytes"), "halves": {}}
        for half, srcs in pool_spec["halves"].items():
            candidates = []  # (corpus rel, group, src path, label, license)
            for label, root, keep, group, license_text in srcs:
                if not root.is_dir():
                    skipped[f"{pool}:{label}:missing"] += 1
                    continue
                for p in walk(args, root):
                    rel = p.relative_to(root).as_posix()
                    if keep(rel):
                        candidates.append((f"{label}/{rel}", f"{label}/{group(rel)}", p, label, license_text))
            verdicts = path_verdicts(args.path_check, [c[0] for c in candidates])
            groups = defaultdict(list)
            for rel, grp, p, label, lic in candidates:
                if verdicts[rel] != "ok":
                    skipped[f"{pool}:path_rejected"] += 1
                    manifest["path_rejected"].append([rel, verdicts[rel]])
                    continue
                try:
                    data = p.read_bytes()
                except OSError:
                    skipped[f"{pool}:unreadable"] += 1
                    continue
                if not data:
                    skipped[f"{pool}:empty"] += 1
                    continue
                if len(data) > pool_spec.get("max_file_bytes", len(data)):
                    skipped[f"{pool}:larger_than_pool_limit"] += 1
                    continue
                sha = digest(data)
                if sha in excluded_hashes:
                    skipped[f"{pool}:in_r2"] += 1
                    continue
                groups[grp].append((rel, len(data), sha, data, label, lic))
            cap_bytes, cap_files = pool_spec["caps"][half]
            order = sorted(groups, key=lambda g: hashlib.sha256(f"{CORPUS}:{g}".encode()).hexdigest())
            rows, total, used_groups, licenses = [], 0, [], {}
            for grp in order:
                members = groups[grp]
                size = sum(m[1] for m in members)
                if len(rows) + len(members) > cap_files or total + size > cap_bytes:
                    continue
                for rel, n, sha, data, label, lic in members:
                    dest = local_root / pool / rel
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(data)
                    rows.append((rel, n, sha, grp))
                    licenses[label] = lic
                total += size
                used_groups.append(grp)
            rows.sort()
            text = "".join(f"{r}\t{s}\t{h}\t{g}\n" for r, s, h, g in rows)
            meta.mkdir(parents=True, exist_ok=True)
            (meta / f"{half}-{pool}.tsv").write_text(text, encoding="utf-8", newline="\n")
            entry["halves"][half] = {
                "dataset_id": f"{half}/{pool}", "sources": [s[0] for s in srcs], "licenses": licenses,
                "caps": {"bytes": cap_bytes, "files": cap_files}, "file_count": len(rows), "bytes": total,
                "blocks": sum((s + CHUNK - 1) // CHUNK for _, s, _, _ in rows), "groups": len(used_groups),
                "groups_available": len(groups), "distinct_content": len({h for _, _, h, _ in rows}),
                "median_file_bytes": sorted(s for _, s, _, _ in rows)[len(rows) // 2] if rows else None,
                "listing": f"{CORPUS}/{half}-{pool}.tsv", "listing_sha256": digest(text.encode())}
            print(pool, half, len(rows), total, len(used_groups), "of", len(groups), flush=True)
        manifest["datasets"][pool] = entry
    manifest["skipped"] = dict(sorted(skipped.items()))
    manifest["path_rejected"].sort()
    manifest["r2_exclusion_check"] = check()
    (ROOT / f"research/datasets/{CORPUS}.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("skipped:", manifest["skipped"])
    print("check:", manifest["r2_exclusion_check"])


def check():
    """Raise if any holdout listing holds content that also occurs in b2r5-corpus-r2."""
    excluded = r2_hashes()
    hashes, files = set(), 0
    for listing in sorted((ROOT / "research/datasets" / CORPUS).glob("*.tsv")):
        for line in listing.read_text(encoding="utf-8").splitlines():
            sha = line.split("\t")[2]
            hashes.add(sha)
            files += 1
    shared = hashes & excluded
    if shared:
        raise AssertionError(f"{len(shared)} holdout content hashes also occur in b2r5-corpus-r2")
    return {"holdout_files": files, "holdout_hashes": len(hashes), "r2_hashes": len(excluded), "shared": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--path-check", help="seedarc_path_check executable")
    parser.add_argument("--django", help="extracted django-6.1.3 directory")
    parser.add_argument("--wsl-root", default=r"\\wsl.localhost\Ubuntu-26.04")
    parser.add_argument("--wsl-user", default=os.environ.get("USERNAME", "").lower())
    parser.add_argument("--wsl-distro", default="Ubuntu-26.04")
    args = parser.parse_args()
    if args.check:
        print(json.dumps(check()))
        return
    if not args.path_check or not args.django:
        parser.error("--path-check and --django are required to build")
    args.path_check = str(Path(args.path_check).resolve(strict=True))
    build(args)


if __name__ == "__main__":
    main()
