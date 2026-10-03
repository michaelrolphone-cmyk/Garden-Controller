#!/usr/bin/env python3
"""Verify a relay deployment ZIP without extracting it or accessing a device."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import zipfile

STORE_FILES = {
    "boot.json",
    "board.json",
    "default.elf",
    "default.json",
    "relay/driver.elf",
    "relay/manifest.json",
    "buzzer/driver.elf",
    "buzzer/manifest.json",
}
GRANTS = [
    {"capability": "switch.relay", "api": 1, "instance_id": 1},
    {"capability": "sound.buzzer", "api": 1, "instance_id": 2},
]
FORBIDDEN_APP_KEYS = {"min_firmware_version", "icon", "category", "status"}
FIRMWARE = "e27d3d089086d79f06edeff4c2bd35f6e6243444"


def xtensa_dyn(elf):
    return elf[:7] == b"\x7fELF\x01\x01\x01" and struct.unpack_from("<HH", elf, 16) == (3, 94)


def verify(path):
    with zipfile.ZipFile(path) as zipped:
        names = zipped.namelist()
        if len(names) != len(set(names)) or any(
                name.startswith("/") or ".." in PurePosixPath(name).parts for name in names):
            raise ValueError("unsafe or duplicate archive path")
        record = json.loads(zipped.read("deployment-record.json"))
        entries = record["entries"]
        if len(entries) != len({entry["path"] for entry in entries}) or set(names) != {
                entry["path"] for entry in entries} | {"deployment-record.json"}:
            raise ValueError("deployment membership mismatch")
        for entry in entries:
            data = zipped.read(entry["path"])
            if len(data) != entry["size_bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError("deployment checksum mismatch")
        if record.get("firmware") != FIRMWARE:
            raise ValueError("deployment is not pinned to the minimal runtime firmware")
        if record.get("physical_verification") != "pending":
            raise ValueError("deployment must not claim a hardware run")
        boot = json.loads(zipped.read("store/boot.json"))
        board = json.loads(zipped.read("store/board.json"))
        app = json.loads(zipped.read("store/default.json"))
        store_names = {name.removeprefix("store/") for name in names if name.startswith("store/")}
        if store_names != STORE_FILES:
            raise ValueError("relay store is not the 8-file closure")
        for relative in store_names:
            if len(("/" + relative).encode()) >= 32:
                raise ValueError("SPIFFS object name exceeds the pinned runtime limit")
        if boot.get("board") != "board.json" or boot.get("default_app") != "default.elf":
            raise ValueError("boot.json paths are not the default launch contract")
        if set(boot) - {"board", "default_app", "drivers", "port", "app_capabilities"}:
            raise ValueError("boot.json has a key the firmware parser rejects")
        if app.get("file_name") != "default.elf" or app.get("entry") != "app_main":
            raise ValueError("default application path mismatch")
        if app.get("type") != "application" or app.get("id") != "garden-relay":
            raise ValueError("default application identity mismatch")
        if app.get("architecture") != "xtensa-esp32s3" or app.get("version") != "0.1.0":
            raise ValueError("default application architecture or version mismatch")
        if FORBIDDEN_APP_KEYS & set(app):
            raise ValueError("app manifest contains a key appPolicies rejects")
        requires = app.get("requires")
        if requires != [
            {"capability": "switch.relay", "api": 1},
            {"capability": "sound.buzzer", "api": 1},
        ]:
            raise ValueError("app requires are not exactly switch.relay@1 and sound.buzzer@1")
        expected_policy = [{"manifest": "default.json", "grants": GRANTS}]
        if boot.get("app_capabilities") != expected_policy:
            raise ValueError("unexpected application grant policy")
        drivers = boot.get("drivers")
        if not isinstance(drivers, list) or len(drivers) != 2:
            raise ValueError("unexpected relay driver closure")
        by_instance = {item["instance_id"]: item["manifest"] for item in drivers}
        if by_instance != {1: "relay/manifest.json", 2: "buzzer/manifest.json"}:
            raise ValueError("drivers are not relay instance 1 and buzzer instance 2")
        devices = {device["instance_id"]: device for device in board["devices"]}
        if not {1, 2, 3, 4} <= set(devices):
            raise ValueError("board.json dropped pixel or wifi devices")
        if board.get("board_id") != "castle-hills-relay6":
            raise ValueError("board.json is not the relay board")
        for instance, manifest_path in by_instance.items():
            manifest = json.loads(zipped.read("store/" + manifest_path))
            if manifest["id"] in {"pixel", "wifi", "garden-relay6"}:
                raise ValueError("selected a driver this firmware slice cannot boot")
            elf = zipped.read("store/" + str(PurePosixPath(manifest_path).parent / manifest["file_name"]))
            if not xtensa_dyn(elf):
                raise ValueError("driver is not a target Xtensa shared ELF")
            device = devices[instance]
            matches = [row for row in manifest.get("hardware_compatibility", [])
                       if row["compatible"] == device["compatible"]
                       and row["config_type"] == device["config_type"]
                       and device["chip"]["revision"] in row["revisions"]]
            if len(matches) != 1:
                raise ValueError("selected driver does not match its board device")
        if not xtensa_dyn(zipped.read("store/default.elf")):
            raise ValueError("application is not a target Xtensa shared ELF")
        return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.archives:
        record = verify(path)
        print(path.name, "verified", record["app_version"], record["board_id"])
