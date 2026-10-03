# Garden RiscRTE packages

Portable ESP32-S3 ELF drivers cover the relay board, CrowPanel dial and paper
board. See [capability contracts, hardware coverage and runtime prerequisites](CAPABILITY_BACKFILL.md).
These packages require proposed low-level capabilities that Reader does not yet
provide. They fail admission when dependencies are unavailable. No hardware
qualification or full Garden app migration is claimed.

```
python3 scripts/test_contracts.py
python3 scripts/build_packages.py --drivers-only
python3 scripts/check_packages.py
```

Install the ESP32-S3 Xtensa toolchain or set `XTENSA_GCC` to its gcc binary.
Driver-only builds need no ESP-IDF SDK because drivers import capability tables,
not GPIO/controller firmware functions. To additionally build the unchanged demo
apps, pass `--firmware-include PATH/TO/T5S3-Reader/lib/NativeApps/include` and omit
`--drivers-only`. The demos must not be used as on-device tests: one starts a
watering zone automatically.

One `.rte.zip` is built per manifest ID, with ABI metadata and generic catalogs.
No release, flash, serial session or launcher is part of this work.
