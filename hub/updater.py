"""Safe GitHub Release updater for Monitor Hub.

Release installs can update in place. Git development checkouts are never overwritten.
Only stable GitHub Releases are used by default.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

DEFAULT_REPO = os.environ.get("MONITOR_HUB_UPDATE_REPO", "stloendays/Monitor")
ASSET_NAME = "monitor-hub-windows-x64.zip"
CHECKSUM_NAME = ASSET_NAME + ".sha256"
USER_AGENT = "MonitorHub-Updater/1"
CACHE_SECONDS = 6 * 60 * 60
_SEMVER = re.compile(
    r"^[vV]?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?$"
)
_REPO = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")


class UpdateError(RuntimeError):
    pass


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def current_version(root: Path | str | None = None) -> str:
    root = Path(root) if root is not None else repo_root()
    try:
        text = (root / "VERSION").read_text(encoding="utf-8").strip()
        parse_semver(text)
        return text.lstrip("vV")
    except (OSError, UpdateError):
        return "0.0.0"


def parse_semver(text: str):
    m = _SEMVER.fullmatch((text or "").strip())
    if not m:
        raise UpdateError(f"invalid semantic version: {text!r}")
    core = tuple(int(m.group(i)) for i in (1, 2, 3))
    pre = tuple(m.group(4).split(".")) if m.group(4) else ()
    return core, pre


def _cmp_pre_identifier(a: str, b: str) -> int:
    a_num, b_num = a.isdigit(), b.isdigit()
    if a_num and b_num:
        return (int(a) > int(b)) - (int(a) < int(b))
    if a_num != b_num:
        return -1 if a_num else 1
    return (a > b) - (a < b)


def compare_versions(a: str, b: str) -> int:
    acore, apre = parse_semver(a)
    bcore, bpre = parse_semver(b)
    if acore != bcore:
        return (acore > bcore) - (acore < bcore)
    if not apre and not bpre:
        return 0
    if not apre:
        return 1
    if not bpre:
        return -1
    for ai, bi in zip(apre, bpre):
        c = _cmp_pre_identifier(ai, bi)
        if c:
            return c
    return (len(apre) > len(bpre)) - (len(apre) < len(bpre))


def is_newer(latest: str, installed: str) -> bool:
    return compare_versions(latest, installed) > 0


def install_mode(root: Path | str | None = None) -> str:
    root = Path(root) if root is not None else repo_root()
    if (root / ".git").exists():
        return "development"
    if (root / ".monitor-hub-release.json").is_file():
        return "release"
    return "unmanaged"


def _api_headers():
    return {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": USER_AGENT,
    }


def _load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return None


def _write_json_atomic(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def _release_from_api(data: dict) -> dict:
    tag = str(data.get("tag_name") or "")
    version = tag.lstrip("vV")
    parse_semver(version)
    assets = []
    for item in data.get("assets") or []:
        if isinstance(item, dict):
            assets.append({
                "name": str(item.get("name") or ""),
                "url": str(item.get("browser_download_url") or ""),
                "size": int(item.get("size") or 0),
            })
    return {
        "tag_name": tag,
        "version": version,
        "name": str(data.get("name") or tag),
        "body": str(data.get("body") or ""),
        "html_url": str(data.get("html_url") or ""),
        "published_at": str(data.get("published_at") or ""),
        "assets": assets,
    }


def get_latest_release(repo: str = DEFAULT_REPO, timeout: float = 8.0):
    if not _REPO.fullmatch(repo):
        raise UpdateError(f"invalid GitHub repository: {repo!r}")
    req = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/releases/latest",
        headers=_api_headers(),
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise UpdateError(f"GitHub release check failed: HTTP {e.code}") from e
    except (urllib.error.URLError, TimeoutError, ValueError) as e:
        raise UpdateError(f"GitHub release check failed: {e}") from e
    if not isinstance(data, dict):
        raise UpdateError("GitHub returned an invalid release payload")
    if data.get("draft") or data.get("prerelease"):
        raise UpdateError("latest release endpoint returned a draft/prerelease unexpectedly")
    return _release_from_api(data)


def _result_for_release(release, *, cached: bool, checked_at: float):
    installed = current_version()
    latest = release.get("version") if isinstance(release, dict) else None
    return {
        "current_version": installed,
        "release": release,
        "available": bool(latest and is_newer(latest, installed)),
        "cached": cached,
        "checked_at": checked_at,
    }


def check_for_update(
    *,
    repo: str = DEFAULT_REPO,
    state_path: Path | str | None = None,
    force: bool = False,
    timeout: float = 8.0,
):
    state_file = Path(state_path) if state_path else None
    now = time.time()
    if state_file and not force:
        state = _load_json(state_file)
        if isinstance(state, dict):
            checked = float(state.get("checked_at") or 0)
            if now - checked < CACHE_SECONDS:
                return _result_for_release(state.get("release"), cached=True, checked_at=checked)

    release = get_latest_release(repo=repo, timeout=timeout)
    if state_file:
        _write_json_atomic(state_file, {"checked_at": now, "repo": repo, "release": release})
    return _result_for_release(release, cached=False, checked_at=now)


def _find_asset(release: dict, name: str) -> dict:
    for asset in release.get("assets") or []:
        if asset.get("name") == name:
            return asset
    raise UpdateError(f"release asset is missing: {name}")


def _allowed_download_url(url: str) -> bool:
    p = urllib.parse.urlparse(url)
    host = (p.hostname or "").lower()
    return p.scheme == "https" and (
        host == "github.com"
        or host == "objects.githubusercontent.com"
        or host == "release-assets.githubusercontent.com"
        or host.endswith(".githubusercontent.com")
    )


def _download(url: str, path: Path, *, timeout: float = 60.0, progress=None) -> None:
    if not _allowed_download_url(url):
        raise UpdateError(f"refusing non-GitHub download URL: {url}")
    path.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response, open(path, "wb") as out:
            total = int(response.headers.get("Content-Length") or 0)
            done = 0
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise UpdateError(f"download failed: {e}") from e


def sha256_file(path: Path | str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def parse_checksum(text: str, filename: str = ASSET_NAME) -> str:
    for raw in text.splitlines():
        m = re.match(r"^([0-9a-fA-F]{64})(?:\s+\*?(.+))?$", raw.strip())
        if m and (not m.group(2) or m.group(2).strip() == filename):
            return m.group(1).lower()
    raise UpdateError("release checksum file is invalid")


def _safe_relpath(text: str) -> Path:
    p = PurePosixPath(text)
    if p.is_absolute() or not p.parts or any(part in ("", ".", "..") for part in p.parts):
        raise UpdateError(f"unsafe path in update manifest: {text!r}")
    return Path(*p.parts)


def safe_extract(zip_path: Path | str, destination: Path | str) -> None:
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    base = destination.resolve()
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            rel = PurePosixPath(info.filename)
            if rel.is_absolute() or any(part == ".." for part in rel.parts):
                raise UpdateError(f"unsafe path in release archive: {info.filename!r}")
            resolved = destination.joinpath(*rel.parts).resolve()
            if base not in (resolved, *resolved.parents):
                raise UpdateError(f"release archive escapes staging directory: {info.filename!r}")
        zf.extractall(destination)


def verify_manifest(stage_dir: Path | str) -> dict:
    stage_dir = Path(stage_dir)
    manifest = _load_json(stage_dir / "manifest.json")
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise UpdateError("missing or unsupported update manifest")
    version = str(manifest.get("version") or "")
    parse_semver(version)
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise UpdateError("update manifest does not list files")
    payload = stage_dir / "payload"
    seen = set()
    for entry in files:
        if not isinstance(entry, dict):
            raise UpdateError("invalid file entry in update manifest")
        rel = _safe_relpath(str(entry.get("path") or ""))
        key = rel.as_posix()
        if key in seen:
            raise UpdateError(f"duplicate manifest path: {key}")
        seen.add(key)
        src = payload / rel
        if not src.is_file():
            raise UpdateError(f"update payload is missing: {key}")
        if src.stat().st_size != int(entry.get("size") or -1):
            raise UpdateError(f"size mismatch for update payload: {key}")
        expected = str(entry.get("sha256") or "").lower()
        if not re.fullmatch(r"[0-9a-f]{64}", expected) or sha256_file(src) != expected:
            raise UpdateError(f"checksum mismatch for update payload: {key}")
    return manifest


def stage_release(release: dict, *, base_dir: Path | str, timeout: float = 60.0, progress=None) -> dict:
    if not isinstance(release, dict):
        raise UpdateError("missing release metadata")
    version = str(release.get("version") or "")
    parse_semver(version)
    zip_asset = _find_asset(release, ASSET_NAME)
    sum_asset = _find_asset(release, CHECKSUM_NAME)

    base = Path(base_dir)
    base.mkdir(parents=True, exist_ok=True)
    work = base / f".tmp-{os.getpid()}-{int(time.time())}"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    try:
        archive = work / ASSET_NAME
        checksum_file = work / CHECKSUM_NAME
        _download(zip_asset["url"], archive, timeout=timeout, progress=progress)
        _download(sum_asset["url"], checksum_file, timeout=timeout)
        expected = parse_checksum(checksum_file.read_text(encoding="utf-8"), ASSET_NAME)
        if sha256_file(archive) != expected:
            raise UpdateError("release archive checksum mismatch")
        extracted = work / "extracted"
        safe_extract(archive, extracted)
        manifest = verify_manifest(extracted)
        if manifest.get("version") != version:
            raise UpdateError("release tag and package manifest version do not match")
        target = base / f"v{version}"
        if target.exists():
            shutil.rmtree(target)
        os.replace(extracted, target)
        return {"stage_dir": str(target), "manifest": manifest, "version": version}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def launch_apply(
    *,
    stage_dir: Path | str,
    install_root: Path | str,
    backup_root: Path | str,
    restart_argv: list[str],
):
    root = Path(install_root)
    mode = install_mode(root)
    if mode != "release":
        raise UpdateError(f"automatic apply is disabled for {mode} installs")
    helper = Path(__file__).with_name("apply_update.py")
    cmd = [
        sys.executable, str(helper),
        "--pid", str(os.getpid()),
        "--stage", str(Path(stage_dir)),
        "--install-root", str(root),
        "--backup-root", str(Path(backup_root)),
        "--restart-json", json.dumps(list(restart_argv), ensure_ascii=False),
    ]
    kwargs = {"cwd": str(root), "close_fds": True}
    if os.name == "nt":
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return subprocess.Popen(cmd, **kwargs)
