#!/usr/bin/env python3
"""Write the licence notices for the Rust code compiled into rt_rig.

Usage: third_party_notices.py <cargo workspace dir> <output dir>
"""
import json
import pathlib
import shutil
import subprocess
import sys

LICENCE_PREFIXES = ("LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE", "UNLICENSE")
RULE = "=" * 78


def run(cwd, *cmd):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout


def licence_files(pkg_dir):
    return sorted(p for p in pkg_dir.iterdir()
                  if p.is_file() and p.name.upper().startswith(LICENCE_PREFIXES))


def main():
    tui_dir = pathlib.Path(sys.argv[1]).resolve()
    out_dir = pathlib.Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    tree = run(tui_dir, "cargo", "tree", "--locked", "--workspace", "-e", "normal",
               "--prefix", "none", "--format", "{p}")
    linked = {(line.split()[0], line.split()[1].lstrip("v")) for line in tree.splitlines() if line.strip()}
    meta = json.loads(run(tui_dir, "cargo", "metadata", "--format-version", "1", "--locked"))
    members = set(meta["workspace_members"])
    packages = sorted((p for p in meta["packages"]
                       if (p["name"], p["version"]) in linked and p["id"] not in members),
                      key=lambda p: (p["name"], p["version"]))

    rustc = run(tui_dir, "rustc", "--version").strip()
    sysroot = pathlib.Path(run(tui_dir, "rustc", "--print", "sysroot").strip())
    shutil.copyfile(sysroot / "share" / "doc" / "rust" / "COPYRIGHT-library.html",
                    out_dir / "RUST-LIBRARY-COPYRIGHT.html")

    parts = [
        "rt_rig contains the Rust standard library and the Rust crates below.\n",
        f"The standard library ({rustc}) notice is in RUST-LIBRARY-COPYRIGHT.html.\n",
        "Each crate's licence files follow, as published in the crate.\n",
    ]
    missing = []
    for p in packages:
        files = licence_files(pathlib.Path(p["manifest_path"]).parent)
        parts.append(f"\n{RULE}\n{p['name']} {p['version']}\nLicence: {p.get('license') or 'see files'}\n")
        if p.get("repository"):
            parts.append(f"Source: {p['repository']}\n")
        for f in files:
            parts.append(f"\n--- {f.name} ---\n\n{f.read_text(encoding='utf-8', errors='replace').rstrip()}\n")
        if not files:
            missing.append(f"{p['name']} {p['version']} ({p.get('license')})")

    (out_dir / "THIRD-PARTY-NOTICES.txt").write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"{len(packages)} crates written to {out_dir / 'THIRD-PARTY-NOTICES.txt'}")
    if missing:
        print("crates with no licence file:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
