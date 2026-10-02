#!/usr/bin/env python3
"""Publish the tag asset and update the release-index branch."""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def asset_for(tag: str) -> Path:
    if tag.startswith("app-"):
        kind, rest = "application", tag[len("app-"):]
    elif tag.startswith("driver-"):
        kind, rest = "driver", tag[len("driver-"):]
    else:
        raise SystemExit(f"unsupported tag {tag}")
    identity, version = rest.rsplit("-v", 1)
    name = f"{kind}-{identity}-{version}-xtensa-esp32s3.rte.zip"
    matches = list((ROOT / "dist").rglob(name))
    if len(matches) != 1:
        raise SystemExit(f"expected one {name}, found {matches}")
    return matches[0]


def main() -> None:
    tag = os.environ["RELEASE_TAG"]
    asset = asset_for(tag)
    subprocess.run([
        "gh", "release", "create", tag, str(asset),
        str(ROOT / "dist/release-packages/package-catalog.json"),
        str(ROOT / "dist/release-app-packages/package-catalog.json"),
        "--verify-tag", "--latest=false", "--title", tag,
        "--notes", "Installable Garden Controller package. Point the app store or driver manager at this repository.",
    ], check=True)
    index = ROOT / "dist/release-index.json"
    subprocess.run(["git", "fetch", "origin", "release-index"], check=False)
    if subprocess.run(["git", "rev-parse", "--verify", "origin/release-index"], capture_output=True).returncode == 0:
        subprocess.run(["git", "checkout", "-B", "release-index", "origin/release-index"], check=True)
    else:
        subprocess.run(["git", "checkout", "--orphan", "release-index"], check=True)
    (ROOT / "release-index.json").write_bytes(index.read_bytes())
    subprocess.run(["git", "add", "release-index.json"], check=True)
    subprocess.run(["git", "-c", "user.email=actions@github.com", "-c", "user.name=github-actions", "commit", "-m", f"Update release index for {tag}"], check=True)
    subprocess.run(["git", "push", "origin", "release-index"], check=True)


if __name__ == "__main__":
    main()
