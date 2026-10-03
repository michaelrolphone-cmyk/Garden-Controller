#!/usr/bin/env python3
"""Build the relay default.elf against the vendored generic runtime header.

Does not link garden_policy. Does not flash.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "riscrte/Apps"


def find_gcc():
    env = os.environ.get("XTENSA_GCC")
    if env:
        return env
    found = shutil.which("xtensa-esp32s3-elf-gcc")
    if found:
        return found
    matches = sorted((Path.home() / ".platformio/packages").glob(
        "toolchain-xtensa-esp32s3*/bin/xtensa-esp32s3-elf-gcc"))
    if matches:
        return str(matches[-1])
    raise SystemExit(
        "xtensa-esp32s3-elf-gcc not found. Install espressif/toolchain-xtensa-esp32s3 or set XTENSA_GCC.")


def main():
    cc = find_gcc()
    out = ROOT / "dist/relay"
    out.mkdir(parents=True, exist_ok=True)
    elf = out / "default.elf"
    exports_map = out / "exports.map"
    exports_map.write_text("{ global: app_main; local: *; };\n")
    subprocess.run([
        cc, "-std=c11", "-Os", "-fPIC", "-mtext-section-literals", "-mlongcalls",
        "-fvisibility=hidden", "-ffreestanding", "-fno-builtin", "-nostdlib", "-nostartfiles",
        "-shared", "-Wl,--hash-style=sysv", "-Wl,--version-script=" + str(exports_map),
        "-Wall", "-Wextra", "-Werror",
        "-I" + str(APP),
        "-I" + str(ROOT / "riscrte/include"),
        str(APP / "garden-relay.c"),
        str(ROOT / "riscrte/include/garden_policy.c"),
        "-lgcc", "-o", str(elf),
    ], check=True)
    readelf = cc.removesuffix("gcc") + "readelf"
    nm = cc.removesuffix("gcc") + "nm"
    header = subprocess.check_output([readelf, "-h", str(elf)], text=True)
    if not all(token in header for token in ("ELF32", "little endian", "Xtensa", "DYN")):
        raise SystemExit("default.elf is not an Xtensa shared ELF")
    symbols = subprocess.check_output([nm, "-D", str(elf)], text=True)
    imports = {line.split()[-1] for line in symbols.splitlines() if " U " in f" {line}"}
    exports = {line.split()[-1] for line in symbols.splitlines()
               if len(line.split()) >= 3 and line.split()[-2] in ("T", "D", "B", "R")}
    if not imports <= {"risc_runtime_get_api", "memcpy", "memset"}:
        raise SystemExit(f"unexpected imports: {sorted(imports)}")
    if "risc_runtime_get_api" not in imports or exports != {"app_main"}:
        raise SystemExit(f"entry/import mismatch: imports={sorted(imports)} exports={sorted(exports)}")
    manifest = json.loads((APP / "garden-relay-boot.json").read_text())
    (out / "default.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (out / "build-record.json").write_text(json.dumps({
        "schema": 1,
        "id": manifest["id"],
        "version": manifest["version"],
        "architecture": "xtensa-esp32s3",
        "artifact": "default.elf",
        "size_bytes": elf.stat().st_size,
        "sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "imports": sorted(imports),
        "linked_objects": ["riscrte/Apps/garden-relay.c", "riscrte/include/garden_policy.c"],
    }, indent=2) + "\n")
    print(f"default.elf {manifest['version']}: target ABI, entry and import checks passed")


if __name__ == "__main__":
    main()
