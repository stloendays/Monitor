"""Apply a staged Monitor Hub release after the running GUI exits."""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import updater


def log_line(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"{dt.datetime.now().isoformat(timespec='seconds')} {text}\n")


def wait_for_pid(pid: int, timeout: float = 120.0) -> None:
    if pid <= 0:
        return
    if os.name == "nt":
        SYNCHRONIZE = 0x00100000
        WAIT_TIMEOUT = 0x00000102
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            return
        try:
            if kernel32.WaitForSingleObject(handle, int(timeout * 1000)) == WAIT_TIMEOUT:
                raise updater.UpdateError("timed out waiting for Monitor Hub to exit")
        finally:
            kernel32.CloseHandle(handle)
        return
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return
        time.sleep(0.2)
    raise updater.UpdateError("timed out waiting for Monitor Hub to exit")


def copy_replace(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + ".monitorhub-update")
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)


def apply(stage: Path, install_root: Path, backup_root: Path, log_path: Path) -> str:
    if updater.install_mode(install_root) != "release":
        raise updater.UpdateError("refusing to update a development or unmanaged directory")
    manifest = updater.verify_manifest(stage)
    version = str(manifest["version"])
    payload = stage / "payload"
    backup = backup_root / (f"v{version}-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S"))
    backup.mkdir(parents=True, exist_ok=True)

    entries = []
    for item in manifest["files"]:
        rel = updater._safe_relpath(str(item["path"]))
        entries.append((rel, payload / rel, install_root / rel))

    existed = {}
    for rel, _src, dst in entries:
        existed[rel.as_posix()] = dst.is_file()
        if dst.is_file():
            target = backup / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(dst, target)

    applied = []
    try:
        for rel, src, dst in entries:
            copy_replace(src, dst)
            applied.append((rel, dst))
    except Exception:
        log_line(log_path, "apply failed; rolling back")
        for rel, dst in reversed(applied):
            old = backup / rel
            try:
                if old.is_file():
                    copy_replace(old, dst)
                elif not existed.get(rel.as_posix(), False) and dst.exists():
                    dst.unlink()
            except OSError as rollback_error:
                log_line(log_path, f"rollback warning for {rel.as_posix()}: {rollback_error}")
        raise

    log_line(log_path, f"updated successfully to {version}; backup={backup}")
    return version


def restart(argv, cwd: Path, log_path: Path) -> None:
    if not isinstance(argv, list) or not argv or not all(isinstance(x, str) and x for x in argv):
        raise updater.UpdateError("invalid restart command")
    kwargs = {"cwd": str(cwd), "close_fds": True}
    if os.name == "nt":
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    subprocess.Popen(argv, **kwargs)
    log_line(log_path, "restart launched")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--install-root", required=True)
    ap.add_argument("--backup-root", required=True)
    ap.add_argument("--restart-json", required=True)
    args = ap.parse_args()

    stage = Path(args.stage).resolve()
    root = Path(args.install_root).resolve()
    backups = Path(args.backup_root).resolve()
    log_path = backups.parent / "update.log"
    try:
        log_line(log_path, f"waiting for pid {args.pid}")
        wait_for_pid(args.pid)
        version = apply(stage, root, backups, log_path)
        restart(json.loads(args.restart_json), root, log_path)
        log_line(log_path, f"finished v{version}")
        return 0
    except Exception as e:  # noqa: BLE001
        log_line(log_path, f"ERROR: {type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
