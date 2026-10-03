#!/usr/bin/env python3
"""Build and unpack-verify a SPIFFS image from one already-verified relay ZIP.

Never flashes. Image size, page, and block match the clock store builder.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
from verify_relay_deployment import verify

SIZE = 0x4f0000


def build(archive, tool, out):
    record = verify(archive)
    out.mkdir(parents=True, exist_ok=True)
    image = out / (archive.stem + "-bootfs.bin")
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "store"
        source.mkdir()
        expected = {}
        with zipfile.ZipFile(archive) as zipped:
            for name in sorted(item for item in zipped.namelist() if item.startswith("store/")):
                relative = name.removeprefix("store/")
                if len(("/" + relative).encode()) >= 32:
                    raise ValueError("SPIFFS object name exceeds the pinned runtime limit")
                data = zipped.read(name)
                expected[relative] = data
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        if len(expected) != 8:
            raise ValueError("relay SPIFFS image must contain exactly 8 store files")
        options = ["-p", "256", "-b", "4096", "-s", str(SIZE)]
        subprocess.run([str(tool), "-c", str(source), *options, str(image)], check=True)
        unpacked = Path(tmp) / "unpacked"
        unpacked.mkdir()
        subprocess.run([str(tool), "-u", str(unpacked), *options, str(image)], check=True)
        actual = {str(path.relative_to(unpacked)): path.read_bytes()
                  for path in unpacked.rglob("*") if path.is_file()}
        if actual != expected or image.stat().st_size != SIZE:
            raise ValueError("SPIFFS round-trip differs from exact deployment store")
    metadata = {
        "schema": 1,
        "board_id": record["board_id"],
        "garden_source_sha": record["source_sha"],
        "firmware": record["firmware"],
        "deployment_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "image": image.name,
        "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "size_bytes": SIZE,
        "partition_label": "bootfs",
        "partition_offset": 0x310000,
        "page_size": 256,
        "block_size": 4096,
        "tool_sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
        "files": len(expected),
        "round_trip_verified": True,
        "hardware_run": False,
        "warning": "Only for the explicitly matching generic 8MiB partition layout; no firmware flash or device verification implied",
    }
    (out / (archive.stem + "-bootfs.json")).write_text(json.dumps(metadata, indent=2) + "\n")
    print("Verified SPIFFS image", image)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", nargs="+", type=Path)
    parser.add_argument("--mkspiffs", required=True, type=Path,
                        help="Pinned PlatformIO tool-mkspiffs 2.230.0 Arduino ESP32 binary")
    parser.add_argument("--output", type=Path, default=Path("dist/relay-images"))
    args = parser.parse_args()
    build(args.archive.resolve(), args.mkspiffs.resolve(), args.output.resolve())
