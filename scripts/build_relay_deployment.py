#!/usr/bin/env python3
"""Stage the relay boot store and an immutable deployment ZIP; never flash.

Driver ELFs come from scripts/build_packages.py --drivers-only
(dist/release-packages/driver-<id>-<version>-xtensa-esp32s3.rte.zip).
Garden packages do not embed source-manifest.json; identity is checked
against .package.json and the in-tree manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = "e27d3d089086d79f06edeff4c2bd35f6e6243444"
# Store directory for each selected hardware instance. Pixel (3), wifi (4),
# and the garden-relay6 catalog ELF are intentionally absent.
SELECTED = {1: "relay", 2: "buzzer"}
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


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def source_sha(root):
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        raise SystemExit("git rev-parse HEAD failed; the deployment record needs the Garden-Controller commit") from exc


def matches(manifest, device):
    revision = device["chip"]["revision"]
    return any(
        row["compatible"] == device["compatible"]
        and row["config_type"] == device["config_type"]
        and revision in row["revisions"]
        for row in manifest.get("hardware_compatibility", [])) and not manifest.get("legacy_manual_only")


def build(root=ROOT):
    profile_path = root / "riscrte/Drivers/garden_relay6/hardware.json"
    board_bytes = profile_path.read_bytes()
    board = json.loads(board_bytes)
    if board.get("schema") != "riscrte.board-hardware" or board.get("board_id") != "castle-hills-relay6":
        raise ValueError("Relay board file is not castle-hills-relay6")
    present = {device["instance_id"] for device in board["devices"]}
    if not {1, 2, 3, 4} <= present:
        raise ValueError("Board file no longer contains relay, buzzer, pixel, and wifi devices")
    app_path = root / "riscrte/Apps/garden-relay-boot.json"
    app = json.loads(app_path.read_text())
    if app.get("file_name") != "default.elf" or app.get("entry") != "app_main":
        raise ValueError("Boot manifest is not the firmware default.elf contract")
    catalog = json.loads((root / "dist/release-packages/package-catalog.json").read_text())["packages"]
    manifests = [json.loads(path.read_text()) for path in (root / "riscrte/Drivers").glob("*/manifest.json")]
    elf_path = root / "dist/relay/default.elf"
    if not elf_path.is_file():
        raise SystemExit("missing dist/relay/default.elf; run scripts/build_relay_app.py first")
    files = {
        "store/default.elf": elf_path.read_bytes(),
        "store/board.json": board_bytes,
        "store/default.json": app_path.read_bytes(),
        "INSTALL.md": (root / "docs/RELAY_INSTALL.md").read_bytes(),
        "source-hardware.json": board_bytes,
    }
    boot = {
        "board": "board.json",
        "default_app": "default.elf",
        "drivers": [],
        "app_capabilities": [{"manifest": "default.json", "grants": GRANTS}],
    }
    selected = []
    # Board order is relay then buzzer. Load order is the provider graph, not this array.
    for device in board["devices"]:
        directory = SELECTED.get(device["instance_id"])
        if directory is None:
            continue
        candidates = [manifest for manifest in manifests if matches(manifest, device)]
        if len(candidates) != 1:
            raise ValueError(f"ambiguous or missing driver for instance {device['instance_id']}")
        manifest = candidates[0]
        if manifest["id"] != directory:
            raise ValueError(f"instance {device['instance_id']} matched {manifest['id']}, not {directory}")
        if manifest["id"] in {"pixel", "wifi", "garden-relay6"}:
            raise ValueError("refusing to select pixel, wifi, or the garden-relay6 catalog ELF")
        package = next(row for row in catalog if row["id"] == manifest["id"])
        archive_path = root / "dist/release-packages" / package["archive"]
        data = archive_path.read_bytes()
        if sha(data) != package["sha256"] or package["version"] != manifest["version"]:
            raise ValueError("stale or corrupt driver package")
        if package["artifact"] != "driver.elf":
            raise ValueError("driver package artifact is not driver.elf")
        with zipfile.ZipFile(archive_path) as archive:
            names = set(archive.namelist())
            if "driver.elf" not in names or ".package.json" not in names:
                raise ValueError("driver package is missing driver.elf or .package.json")
            packaged = json.loads(archive.read(".package.json"))
            if packaged.get("id") != manifest["id"] or packaged.get("version") != manifest["version"]:
                raise ValueError("packaged identity differs from the source manifest")
            if packaged.get("driver_abi") != manifest["driver_abi"] or packaged.get("provides") != manifest["provides"]:
                raise ValueError("packaged capability differs from the source manifest")
            elf = archive.read("driver.elf")
        prefix = "store/" + directory
        files[prefix + "/driver.elf"] = elf
        files[prefix + "/manifest.json"] = (root / "riscrte/Drivers" / directory / "manifest.json").read_bytes()
        files["packages/" + package["archive"]] = data
        boot["drivers"].append({
            "manifest": directory + "/manifest.json",
            "instance_id": device["instance_id"],
        })
        selected.append({
            "id": package["id"],
            "version": package["version"],
            "archive": package["archive"],
            "sha256": package["sha256"],
            "instance_id": device["instance_id"],
        })
    if {item["instance_id"] for item in boot["drivers"]} != {1, 2} or len(boot["drivers"]) != 2:
        raise ValueError("boot store must select only relay instance 1 and buzzer instance 2")
    files["store/boot.json"] = encoded(boot)
    store_names = {name.removeprefix("store/") for name in files if name.startswith("store/")}
    if store_names != STORE_FILES:
        raise ValueError(f"store file set {sorted(store_names)} is not the 8-file relay closure")
    for relative in store_names:
        if len(("/" + relative).encode()) >= 32:
            raise ValueError(f"SPIFFS object name too long: /{relative}")
    record = {
        "schema": "riscrte.garden-relay-deployment",
        "schema_version": 1,
        "app_id": "garden-relay",
        "app_version": app["version"],
        "source_sha": source_sha(root),
        "firmware": FIRMWARE,
        "board_id": board["board_id"],
        "profile": board["revision"],
        "physical_verification": "pending",
        "drivers": selected,
        "omitted_devices": [3, 4],
        "transformations": [
            "Keep every device in riscrte/Drivers/garden_relay6/hardware.json",
            "Select only buzzer instance 2 and relay instance 1",
            "Do not select pixel, wifi, or the garden-relay6 catalog ELF",
            "Grant switch.relay@1 instance 1 and sound.buzzer@1 instance 2",
            "Leave every relay and buzzer channel off",
        ],
        "entries": [
            {"path": name, "size_bytes": len(data), "sha256": sha(data)}
            for name, data in sorted(files.items())
        ],
    }
    files["deployment-record.json"] = encoded(record)
    out = root / "dist/relay-deployments" / board["board_id"]
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    archive_name = f"garden-relay-{app['version']}-{board['board_id']}.zip"
    archive_path = out.parent / archive_name
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_STORED) as zipped:
        for path, data in sorted(files.items()):
            info = zipfile.ZipInfo(path, (2026, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            zipped.writestr(info, data)
    return {
        "board_id": board["board_id"],
        "archive": archive_name,
        "sha256": sha(archive_path.read_bytes()),
        "source_sha": record["source_sha"],
        "firmware": FIRMWARE,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="Garden-Controller checkout (default: this repo)")
    args = parser.parse_args()
    root = args.root.resolve()
    catalog = {"schema": 1, "deployments": [build(root)]}
    destination = root / "dist/relay-deployments"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "catalog.json").write_bytes(encoded(catalog))
    print(f"Built {catalog['deployments'][0]['archive']}; no hardware access and no flash")


if __name__ == "__main__":
    main()
