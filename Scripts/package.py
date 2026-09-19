#!/usr/bin/env python3
"""Assemble an unpublished, deterministic ZIP from completed local builds.
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import hashlib
import os
from pathlib import Path
import time
import tempfile
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {"Build", "Toolchain", ".git", ".codex", ".agents", "__pycache__", "build"}

def editor_temporary(path):
    return (path.name.startswith((".~lock.", ".#")) or path.name.endswith("~") or
            path.suffix in (".swp", ".swo"))

def safe_path(path):
    path = Path(os.path.abspath(path))
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError(f"Refusing symbolic-link path: {path}")
    return path


def source_files():
    manifest = safe_path(ROOT / "SOURCE_MANIFEST.txt")
    names = manifest.read_text().splitlines()
    if not names or names != sorted(set(names)) or "SOURCE_MANIFEST.txt" not in names:
        raise ValueError("Source manifest must be sorted, unique, and include itself")
    selected = set()
    forbidden = {".bin", ".pem", ".key", ".debug", ".map", ".efi", ".img", ".qcow2", ".log", ".env"}
    for name in names:
        relative = Path(name)
        if (relative.is_absolute() or ".." in relative.parts or relative.as_posix() != name or
                any(part in EXCLUDED for part in relative.parts) or
                relative.suffix.lower() in forbidden or
                relative.name.lower().startswith(("config.ini", "probe.ini", ".env"))):
            raise ValueError(f"Forbidden source manifest entry: {name}")
        path = safe_path(ROOT / relative)
        if not path.is_file():
            raise ValueError(f"Missing source manifest file: {name}")
        selected.add(path)
    for path in ROOT.rglob("*"):
        relative = path.relative_to(ROOT)
        if any(part in EXCLUDED for part in relative.parts):
            continue
        safe_path(path)
        if path.is_file() and path not in selected:
            raise ValueError(f"Unreviewed source file: {relative}; update the reviewed manifest or move it out")
    return sorted(selected)


def archive(output, files):
    # A caller can pin this to a release epoch; ZIP cannot represent pre-1980 times.
    epoch = max(315532800, int(os.environ.get("SOURCE_DATE_EPOCH", "315532800")))
    stamp = time.gmtime(epoch)[:6]
    output = safe_path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = tempfile.NamedTemporaryFile(prefix=".ncv-zip-", dir=output.parent, delete=False)
    staged_path = Path(staging.name)
    staging.close()
    try:
        write_archive(staged_path, files, stamp)
        with staged_path.open("rb") as stream:
            os.fsync(stream.fileno())
        os.replace(staged_path, output)
    finally:
        staged_path.unlink(missing_ok=True)
    print(output)


def write_archive(output, files, stamp):
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, data, executable in sorted(files):
            info = zipfile.ZipInfo(name, stamp)
            info.create_system = 3
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(info, data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "Build/Packages")
    args = parser.parse_args()
    args.output = safe_path(args.output)
    if args.output == ROOT or ROOT.is_relative_to(args.output):
        raise ValueError("Package output cannot replace a source ancestor")
    if args.output.is_relative_to(ROOT) and not args.output.is_relative_to(ROOT / "Build"):
        raise ValueError("Inside the source tree, package output must be under Build")
    reviewed = source_files()
    # Validate every destination before creating even the first ZIP.
    for name in ("NativeCsmVgaSelector-1.2-source.zip", "NativeCsmVgaSelector-1.2.zip", "NativeCsmVgaSelector-1.2"):
        safe_path(args.output / name)
    # Source ZIP includes tests, build instructions, and notices; no runtime config.
    source = [(f"NativeCsmVgaSelector-1.2/{p.relative_to(ROOT).as_posix()}", p.read_bytes(), bool(p.stat().st_mode & 0o111)) for p in reviewed]
    archive(args.output / "NativeCsmVgaSelector-1.2-source.zip", source)
    payload = {
        "NativeCsmVgaSelector-1.2.efi": ROOT / "Build/Release/BOOTX64.EFI",
        "NativeCsmVgaSelector-1.2-Debug.efi": ROOT / "Build/Debug/BOOTX64.EFI",
        "BOOTX64.EFI": ROOT / "Build/Release/BOOTX64.EFI",
        "pci.ids": ROOT / "Data/pci.ids",
        "install.sh": ROOT / "Installers/Linux/install.sh",
        "launch.sh": ROOT / "Installers/Linux/launch.sh",
        "NativeCsmVgaSelector-Installer.desktop": ROOT / "Installers/Linux/NativeCsmVgaSelector-Installer.desktop",
        "Installer.ps1": ROOT / "Installers/Windows/Installer.ps1",
        "LICENSE": ROOT / "LICENSE",
        "THIRD_PARTY_NOTICES.md": ROOT / "THIRD_PARTY_NOTICES.md",
        "README.md": ROOT / "README.md",
    }
    for p in reviewed:
        relative = p.relative_to(ROOT)
        if relative.parts[0] in ("Docs", "LICENSES", "Data") and p.name != "pci.ids":
            payload[relative.as_posix()] = p
        elif relative.parent == Path("Installers/Windows") and p.suffix == ".cmd":
            payload[p.name] = p
    for p in payload.values():
        safe_path(p)
    files = [(name, p.read_bytes(), p.suffix in (".sh", ".desktop")) for name, p in payload.items()]
    manifest = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data, _ in sorted(files))
    files.append(("SHA256SUMS", manifest.encode(), False))
    archive(args.output / "NativeCsmVgaSelector-1.2.zip", files)
    # Stage the complete runtime tree. Refuse to delete an existing directory
    # with unexpected or user-edited files; generated output is not a scratchpad.
    runtime = args.output / "NativeCsmVgaSelector-1.2"
    if runtime.exists():
        existing = {p.relative_to(runtime).as_posix() for p in runtime.rglob("*") if p.is_file()}
        expected = {name for name, _, _ in files}
        for p in runtime.rglob("*"):
            safe_path(p)
        old_manifest = runtime / "SHA256SUMS"
        if not old_manifest.is_file() or existing != expected:
            raise ValueError("Existing runtime contains unexpected files; choose a fresh output directory")
        for line in old_manifest.read_text().splitlines():
            digest, name = line.split("  ", 1)
            if name not in existing or hashlib.sha256((runtime / name).read_bytes()).hexdigest() != digest:
                raise ValueError("Existing runtime was edited; choose a fresh output directory")
    stage = Path(tempfile.mkdtemp(prefix=".ncv-runtime-", dir=args.output))
    previous = args.output / (stage.name + ".previous")
    try:
        for name, data, executable in files:
            destination = stage / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("xb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            destination.chmod(0o755 if executable else 0o644)
        if runtime.exists():
            os.replace(runtime, previous)
        try:
            os.replace(stage, runtime)
        except Exception:
            if previous.exists():
                os.replace(previous, runtime)
            raise
        if previous.exists():
            shutil.rmtree(previous)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print(runtime)

if __name__ == "__main__":
    main()
