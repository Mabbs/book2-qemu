#!/usr/bin/env python3
"""Assemble a relocatable UCRT QEMU runtime. Does not build or launch QEMU.

Standard library only. Run only after the executable/build has stopped changing.
Images and hardware sources are deliberately excluded.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = Path(r"C:\msys64\home\Mayx\book2\qemu-11.1.3")
IMPORT_RE = re.compile(r"DLL Name:\s*(\S+)", re.IGNORECASE)


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def imports(path: Path, objdump: Path) -> list[str]:
    result = subprocess.run([str(objdump), "-p", str(path)], check=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace")
    return IMPORT_RE.findall(result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--msys-root", type=Path, default=Path(r"C:\msys64"))
    parser.add_argument("--output", type=Path, default=PROJECT / "dist")
    args = parser.parse_args()
    exe = (args.exe or args.qemu_source / "build/qemu-system-mipsel.exe").resolve()
    prefix = args.msys_root / "ucrt64"
    objdump = prefix / "bin/objdump.exe"
    output = args.output.resolve()
    if not exe.is_file() or not objdump.is_file():
        parser.error(f"Missing executable or objdump: {exe}; {objdump}")
    if output.exists():
        parser.error("Output already exists; select a new --output directory. No existing dist is overwritten.")
    protected_roots = (args.qemu_source.resolve(), prefix.resolve())
    if (output == PROJECT or output in PROJECT.parents or output == exe.parent
            or any(output == root or root in output.parents for root in protected_roots)):
        parser.error("Unsafe output directory: keep package output outside QEMU/MSYS2 trees")
    # Do not combine a moving executable and stale dependency set.
    initial_exe_hash = digest(exe)
    output.mkdir(parents=True)
    copied: dict[str, str] = {}
    queue: list[Path] = []
    processed: set[Path] = set()
    provenance: dict[str, str] = {}
    search_directories = [exe.parent, prefix / "bin"]
    windows = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    system_directories = [windows / "System32", windows / "SysWOW64"]

    def copy_file(source: Path, relative: Path) -> Path:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and digest(target) != digest(source):
            raise RuntimeError(f"Conflicting package destination: {relative}")
        shutil.copy2(source, target)
        provenance[relative.as_posix()] = str(source)
        return target

    def copy_tree(relative: str, *, required: bool = False) -> None:
        source = prefix / relative
        if not source.is_dir():
            if required:
                raise RuntimeError(f"Required GTK runtime data missing: {source}")
            return
        for item in source.rglob("*"):
            if item.is_file():
                destination = Path(relative) / item.relative_to(source)
                copy_file(item, destination)
                if item.suffix.lower() in (".dll", ".exe"):
                    queue.append(item)

    def add_binary(source: Path) -> None:
        key = source.name.lower()
        source_hash = digest(source)
        if key in copied:
            if copied[key] != source_hash:
                raise RuntimeError(f"Conflicting DLL basename: {source.name}")
            return
        copied[key] = source_hash
        copy_file(source, Path("bin") / source.name)
        queue.append(source)

    try:
        add_binary(exe)
        # LoadLibrary modules are not visible in the main executable's import table.
        build = exe.parent
        module_locations = [build / "ui", build / "audio", build / "accel", build / "hw", build / "block"]
        for location in module_locations:
            if location.is_dir():
                for module in location.rglob("*.dll"):
                    add_binary(module)
        loader_query = prefix / "bin/gdk-pixbuf-query-loaders.exe"
        if loader_query.is_file():
            add_binary(loader_query)
        for relative in ("share/glib-2.0/schemas", "share/gtk-3.0", "share/icons", "share/mime"):
            copy_tree(relative, required=True)
        for relative in ("share/themes", "share/fonts", "share/fontconfig", "etc/fonts", "etc/gtk-3.0",
                         "lib/gtk-3.0", "lib/gdk-pixbuf-2.0", "lib/gio/modules"):
            copy_tree(relative)
        firmware = args.qemu_source / "pc-bios"
        if not firmware.is_dir():
            raise RuntimeError(f"QEMU pc-bios directory missing: {firmware}")
        for item in firmware.rglob("*"):
            if item.is_file():
                copy_file(item, Path("share/qemu") / item.relative_to(firmware))
        unresolved: set[str] = set()
        while queue:
            binary = queue.pop()
            binary = binary.resolve()
            if binary in processed:
                continue
            processed.add(binary)
            for name in imports(binary, objdump):
                key = name.lower()
                if key in copied:
                    continue
                # UCRT/API-set contracts are supplied by Windows, never vendored.
                if key.startswith(("api-ms-win-", "ext-ms-win-")):
                    continue
                candidates = [directory / name for directory in [binary.parent] + search_directories]
                dependency = next((p for p in candidates if p.is_file()), None)
                if dependency is not None:
                    add_binary(dependency)
                elif not any((directory / name).is_file() for directory in system_directories):
                    unresolved.add(f"{binary.name}: {name}")
        if unresolved:
            raise RuntimeError("Unresolved PE imports:\n" + "\n".join(sorted(unresolved)))
        if digest(exe) != initial_exe_hash:
            raise RuntimeError("QEMU executable changed during packaging; rebuild must be idle.")
        for name in ("start.ps1", "start.cmd"):
            copy_file(PROJECT / name, Path(name))
        documentation = PROJECT / "analysis/packaging/README.md"
        if documentation.is_file():
            copy_file(documentation, Path("PACKAGING.md"))
        manifest = {
            "schema": 1, "qemu_sha256": initial_exe_hash,
            "files": [{"path": p.relative_to(output).as_posix(), "sha256": digest(p),
                       "source": provenance.get(p.relative_to(output).as_posix(), "")}
                      for p in sorted(output.rglob("*")) if p.is_file()],
            "note": "No image included; no QEMU startup or functional validation performed."
        }
        (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Packaged {len(manifest['files'])} files into {output}")
        return 0
    except Exception:
        # Keep evidence for diagnosis; never advertise an incomplete directory as ready.
        (output / "INCOMPLETE.txt").write_text("Packaging failed; do not distribute. See command error.\n", encoding="utf-8")
        raise


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        raise SystemExit(1)
