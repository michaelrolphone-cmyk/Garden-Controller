#!/usr/bin/env python3
"""Build Garden Controller app and driver packages for RiscRTE releases."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCH = "xtensa-esp32s3"
APPS = ("garden-relay", "garden-encoder")
DRIVERS = tuple(sorted(p.parent.name for p in (ROOT / "riscrte/Drivers").glob("*/manifest.json")))


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def find_gcc() -> str:
    env = os.environ.get("XTENSA_GCC")
    if env:
        return env
    found = shutil.which("xtensa-esp32s3-elf-gcc")
    if found:
        return found
    matches = sorted((Path.home() / ".platformio/packages").glob("toolchain-xtensa-esp32s3*/bin/xtensa-esp32s3-elf-gcc"))
    if matches:
        return str(matches[-1])
    raise SystemExit("xtensa-esp32s3-elf-gcc not found. Install espressif/toolchain-xtensa-esp32s3 or set XTENSA_GCC.")


def compile_elf(gcc: str, sources: list[Path], output: Path, app: bool, includes: list[Path]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        gcc, "-std=c11", "-Os", "-fPIC", "-mtext-section-literals", "-mlongcalls",
        "-fvisibility=hidden", "-nostdlib", "-nostartfiles", "-shared",
        "-Wall", "-Wextra",
        f"-I{ROOT / 'riscrte/sdk'}",
        f"-I{ROOT / 'riscrte/include'}",
        *[f"-I{path}" for path in includes],
        "-Wl,--hash-style=sysv", "-Wl,--allow-shlib-undefined",
        *[str(path) for path in sources],
        "-lgcc", "-o", str(output),
    ]
    subprocess.run(cmd, check=True)
    symbols = subprocess.run(
        [str(Path(gcc).with_name("xtensa-esp32s3-elf-readelf")), "--dyn-syms", "--wide", str(output)],
        check=True, capture_output=True, text=True,
    ).stdout
    required = "app_main" if app else "t5_driver_get"
    if not any(required == line.split()[-1] and " UND " not in line for line in symbols.splitlines() if line.split()):
        raise SystemExit(f"{output} does not export {required}")

    if not app:
        undefined = {line.split()[-1] for line in symbols.splitlines()
                     if " UND " in line and len(line.split()) >= 8}
        unexpected = undefined - {"memcpy", "memset", "memcmp", "strcmp", "strlen", "strncpy"}
        if unexpected:
            raise SystemExit(f"{output}: unexpected driver imports: {sorted(unexpected)}")


def stored_zip(members: list[tuple[str, bytes]]) -> bytes:
    local = bytearray()
    central = bytearray()
    for name, payload in members:
        encoded = name.encode("ascii")
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        local_offset = len(local)
        local += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0, crc, len(payload), len(payload), len(encoded), 0)
        local += encoded + payload
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0, crc, len(payload), len(payload), len(encoded), 0, 0, 0, 0, 0, local_offset)
        central += encoded
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members), len(members), len(central), len(local), 0)
    return bytes(local + central + eocd)


def write_package(kind: str, source: dict, files: dict[str, bytes], out_dir: Path) -> dict:
    identity = source["id"]
    version = source["version"]
    requires = [{"capability": item["capability"], "min_api": item["api"]} for item in source.get("requires", [])]
    files = dict(files)
    if kind == "driver":
        capability = source["provides"][0]
        files["provider-abi.v1"] = f"os-cpu-abi=1\nprovides={capability['capability']}\napi={capability['api']}\n".encode("ascii")
    else:
        elf = files[source["file_name"]]
        sidecar = dict(source)
        sidecar["size_bytes"] = len(elf)
        sidecar["sha256"] = sha256(elf)
        files[f"{identity}.json"] = (json.dumps(sidecar, indent=2) + "\n").encode("ascii")
    entries = [
        {"name": name, "size_bytes": len(payload), "sha256": sha256(payload), "executable": name.endswith(".elf")}
        for name, payload in files.items()
    ]
    manifest = {
        "schema": 1,
        "kind": kind,
        "id": identity,
        "version": version,
        "architecture": ARCH,
        "artifact": source["file_name"],
        "requires": requires,
        "entries": entries,
    }
    if kind == "driver":
        manifest["driver_abi"] = source["driver_abi"]
        manifest["provides"] = source["provides"]
        for key in ("hardware_compatibility", "hardware_manifest"):
            if key in source: manifest[key] = source[key]
    blob = stored_zip([(".package.json", (json.dumps(manifest, indent=2) + "\n").encode("ascii")), *files.items()])
    prefix = "application" if kind == "application" else "driver"
    name = f"{prefix}-{identity}-{version}-{ARCH}.rte.zip"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / name).write_bytes(blob)
    return {
        "kind": kind,
        "id": identity,
        "version": version,
        "artifact": source["file_name"],
        "architecture": ARCH,
        "archive": name,
        "size_bytes": len(blob),
        "sha256": sha256(blob),
        "tag": f"{'app' if kind == 'application' else 'driver'}-{identity}-v{version}",
    }


def build(gcc: str, firmware_include: Path, drivers_only: bool = False) -> None:
    subprocess.run([os.sys.executable, str(ROOT / "scripts/hardware_manifests.py")], check=True)
    rows = []
    driver_dir = ROOT / "dist/release-packages"
    app_dir = ROOT / "dist/release-app-packages"
    includes = [firmware_include, ROOT / "riscrte/Drivers"]
    seen_ids = set()
    for name in DRIVERS:
        source_dir = ROOT / "riscrte/Drivers" / name
        manifest_path = source_dir / "manifest.json"
        if not manifest_path.exists():
            raise SystemExit(f"Missing manifest: {manifest_path}")
        source = load_json(manifest_path)
        if source.get("file_name") != "driver.elf" or source["id"] in seen_ids:
            raise SystemExit(f"Invalid artifact or duplicate package ID: {manifest_path}")
        seen_ids.add(source["id"])
        elf = driver_dir / name / "driver.elf"
        compile_elf(gcc, [source_dir / "driver.c"], elf, False, includes)
        files = {"driver.elf": elf.read_bytes()}
        if "hardware_manifest" in source:
            files[source["hardware_manifest"]] = (source_dir / source["hardware_manifest"]).read_bytes()
        rows.append(write_package("driver", source, files, driver_dir))
    policy = ROOT / "riscrte/include/garden_policy.c"
    for name in (() if drivers_only else APPS):
        source = load_json(ROOT / "riscrte/Apps" / f"{name}.json")
        elf = app_dir / name / source["file_name"]
        compile_elf(gcc, [ROOT / "riscrte/Apps" / f"{name}.c", policy], elf, True, includes)
        rows.append(write_package("application", source, {source["file_name"]: elf.read_bytes()}, app_dir))
    for directory, kind in ((driver_dir, "driver"), (app_dir, "application")):
        directory.mkdir(parents=True, exist_ok=True)
        catalog = {"schema": 1, "release": os.environ.get("RISC_PACKAGE_RELEASE", "garden-controller"), "packages": [row for row in rows if row["kind"] == kind]}
        (directory / "package-catalog.json").write_text(json.dumps(catalog, indent=2) + "\n")
    index = {
        "schema": 1,
        "repository": "michaelrolphone-cmyk/Garden-Controller",
        "apps": [{"id": row["id"], "version": row["version"], "tag": row["tag"], "asset": row["archive"]} for row in rows if row["kind"] == "application"],
        "drivers": [{"id": row["id"], "version": row["version"], "tag": row["tag"], "asset": row["archive"]} for row in rows if row["kind"] == "driver"],
    }
    (ROOT / "dist/release-index.json").write_text(json.dumps(index, indent=2) + "\n")
    for row in rows:
        print(f"{row['tag']} {row['archive']}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default="")
    parser.add_argument("--drivers-only", action="store_true")
    parser.add_argument("--firmware-include", default=os.environ.get("FIRMWARE_INCLUDE", ""))
    args = parser.parse_args()
    include = Path(args.firmware_include) if args.firmware_include else ROOT / "third_party/T5S3-Reader/lib/NativeApps/include"
    if not args.drivers_only and not (include / "T5AppApi.h").exists():
        raise SystemExit(f"missing T5AppApi.h in {include}. Checkout T5S3-Reader headers there or pass --firmware-include.")
    build(args.gcc or find_gcc(), include, args.drivers_only)


if __name__ == "__main__":
    main()
