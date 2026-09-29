"""Build the portable Windows release package consumed by hub/updater.py."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import tempfile
import zipfile
from pathlib import Path

ASSET = "monitor-hub-windows-x64.zip"
TAG_RE = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def collect(repo: Path):
    paths = []
    for name in ("VERSION", "README.md"):
        p = repo / name
        if not p.is_file():
            raise SystemExit(f"required release file missing: {name}")
        paths.append((p, Path(name)))
    for pattern in ("hub/*.py", "hub/*.md", "hub/*.json", "docs/*.md", "docs/images/*", "deps/*.ps1"):
        for p in sorted(repo.glob(pattern)):
            if p.is_file():
                rel = p.relative_to(repo)
                if rel.as_posix() != "hub/monitor_hub_projects.json":
                    paths.append((p, rel))
    return paths


def write_manifest(payload: Path, version: str, output: Path) -> None:
    files = []
    all_files = sorted((x for x in payload.rglob("*") if x.is_file()), key=lambda x: x.relative_to(payload).as_posix())
    for p in all_files:
        rel = p.relative_to(payload).as_posix()
        files.append({"path": rel, "size": p.stat().st_size, "sha256": sha256(p)})
    output.write_text(json.dumps({"format": 1, "version": version, "files": files}, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", default="dist")
    ap.add_argument("--tag", required=True)
    args = ap.parse_args()

    repo = Path(args.repo_root).resolve()
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    if not TAG_RE.fullmatch(args.tag.strip()):
        raise SystemExit(f"release tag must be vMAJOR.MINOR.PATCH, got {args.tag!r}")
    version = args.tag[1:]
    file_version = (repo / "VERSION").read_text(encoding="utf-8").strip()
    if file_version != version:
        raise SystemExit(f"VERSION={file_version!r} does not match tag {args.tag!r}")
    if not exe.is_file():
        raise SystemExit(f"built CLI not found: {exe}")

    out.mkdir(parents=True, exist_ok=True)
    archive = out / ASSET
    checksum = out / (ASSET + ".sha256")

    with tempfile.TemporaryDirectory(prefix="monitor-hub-release-") as td:
        root = Path(td)
        payload = root / "payload"
        payload.mkdir()
        for src, rel in collect(repo):
            dst = payload / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        bin_dst = payload / "bin" / "monitor_hub_cli.exe"
        bin_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe, bin_dst)
        (payload / ".monitor-hub-release.json").write_text(
            json.dumps({"format": 1, "version": version}, indent=2), encoding="utf-8"
        )
        manifest = root / "manifest.json"
        write_manifest(payload, version, manifest)
        if archive.exists():
            archive.unlink()
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            zf.write(manifest, "manifest.json")
            files = sorted((x for x in payload.rglob("*") if x.is_file()), key=lambda x: x.relative_to(payload).as_posix())
            for p in files:
                zf.write(p, "payload/" + p.relative_to(payload).as_posix())

    digest = sha256(archive)
    checksum.write_text(f"{digest}  {ASSET}\n", encoding="utf-8")
    print(archive)
    print(checksum)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
