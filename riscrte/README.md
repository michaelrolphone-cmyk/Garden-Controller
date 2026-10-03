# Garden RiscRTE packages

Portable ESP32-S3 ELF drivers cover the relay board, CrowPanel dial and paper
board. See [capability contracts, hardware coverage and runtime prerequisites](CAPABILITY_BACKFILL.md).
These packages require proposed low-level capabilities that Reader does not yet
provide. They fail admission when dependencies are unavailable. No hardware
qualification or full Garden app migration is claimed.

```
python3 scripts/test_hardware_manifests.py
python3 scripts/test_contracts.py
python3 scripts/build_packages.py --drivers-only
python3 scripts/check_packages.py
```

Install the ESP32-S3 Xtensa toolchain or set `XTENSA_GCC` to its gcc binary.
Driver-only builds need no ESP-IDF SDK because drivers import capability tables,
not GPIO/controller firmware functions. To additionally build the unchanged demo
apps, pass `--firmware-include PATH/TO/T5S3-Reader/lib/NativeApps/include` and omit
`--drivers-only`. The encoder demo is unchanged and must not be used for an
on-device test. The relay boot app does not energize a channel; see
[the relay install note](../docs/RELAY_INSTALL.md).

One `.rte.zip` is built per manifest ID, with ABI metadata and generic catalogs.
No release, flash, or serial session is part of this work. The relay boot store
is host-built and not flashed. See [the relay install note](../docs/RELAY_INSTALL.md).

Board catalogs describe physical devices and wiring; reusable chip ELFs receive
`hardware.device@1` typed configurations. See the [shared mapping proposal](hardware/CONTRACT.md).
The relay boot store is host-built for generic RiscRTE
`e27d3d089086d79f06edeff4c2bd35f6e6243444` and is not flashed. See
[the relay install note](../docs/RELAY_INSTALL.md). Encoder and e-ink apps are unchanged.
