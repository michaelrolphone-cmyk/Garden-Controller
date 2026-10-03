# Garden relay software candidate

This is a relay-board application and two selected external ELF drivers for the
generic RiscRTE runtime. It is not a successful hardware-test report. No script
in this repository flashes, resets, erases, or accesses a serial device.

The firmware pin is RiscRTE `feat/minimal-runtime`
[`e27d3d089086d79f06edeff4c2bd35f6e6243444`](https://github.com/michaelrolphone-cmyk/RiscRTE/commit/e27d3d089086d79f06edeff4c2bd35f6e6243444).
The boot store is a flat SPIFFS tree. The firmware does not open the outer ZIP.

`risc_runtime_get_api` is declared by `riscrte/Apps/RiscRuntimeV1.h`, copied
from `michaelrolphone-cmyk/RiscRTE` `sdk/app/RiscRuntimeV1.h` at that firmware
commit. The same bytes are `sdk/app/RiscRuntimeV1.h` in
`michaelrolphone-cmyk/RiscRTE-T-Watch-S3` at `codex/watch-clock-app`
`3157fdc6f0e65caa3f81e5f3305927b2b27ca675`.

## Build the application and deployment

Use the ESP32-S3 GCC 8.4.0 esp2021r2-patch5 toolchain. `XTENSA_GCC` can select
that compiler. Driver ELFs come from the existing package builder; this path
does not invent a second driver ABI.

```sh
python3 scripts/build_packages.py --drivers-only
python3 scripts/check_packages.py
python3 scripts/build_relay_app.py
python3 scripts/build_relay_deployment.py
python3 scripts/verify_relay_deployment.py dist/relay-deployments/garden-relay-0.1.0-castle-hills-relay6.zip
```

`build_relay_app.py` compiles `riscrte/Apps/garden-relay.c` and
`riscrte/include/garden_policy.c`. The app is the existing relay program.
It acquires `switch.relay` and `sound.buzzer`, starts zone 1 for the default
run, chirps the buzzer, and ticks the same policy once a second. The runtime
call is `risc_runtime_get_api` instead of the old T5 app and provider calls.
There is no button poll on this runtime, so the loop does not watch for Back.

The authoritative ZIP is
`dist/relay-deployments/garden-relay-0.1.0-castle-hills-relay6.zip`. Its
`store/` tree is the boot store:

- `boot.json`: `default.elf`, drivers for relay instance 1 and buzzer instance 2,
  and grants for `switch.relay` api 1 instance 1 and `sound.buzzer` api 1 instance 2.
- `board.json`: the bytes of `riscrte/Drivers/garden_relay6/hardware.json`.
  Pixel and Wi-Fi devices stay in the board file and are not selected.
- `default.elf` and `default.json`: the boot app. `default.json` is
  `riscrte/Apps/garden-relay-boot.json`.
- `relay/driver.elf`, `relay/manifest.json`, `buzzer/driver.elf`,
  `buzzer/manifest.json`: ELFs taken from the `.rte.zip` files
  `build_packages.py --drivers-only` wrote, next to their source manifests.

The garden-relay6 catalog ELF, the pixel driver, and the Wi-Fi driver are not
in `boot.json`. This firmware slice does not provide the waveform output or
`platform.radio` those drivers need, and it does not boot through the catalog
ELF.

`packages/` keeps the original relay and buzzer `.rte.zip` files.
`deployment-record.json` records the size and SHA-256 of every payload, the
Garden source SHA, and the firmware pin. The versioned ZIP is authoritative;
the adjacent extracted tree is only a build convenience. Check
`dist/relay-deployments/catalog.json`, then verify the inner files before
installation. Do not merge this store into a stale tree.

Garden driver zips contain `driver.elf` and `.package.json`. They do not
contain `source-manifest.json`. The deployment script checks the package
identity against `.package.json` and copies `driver.elf` out of the zip.

## Runtime and store installation contract

The current generic runtime's example partition table is an **8MiB layout**:
factory app offset 0x10000 size 0x300000; bootfs SPIFFS offset 0x310000 size
0x4f0000. Do not assume the relay board already has this partition table.
Confirm the exact runtime build and partition table before any separately
performed hardware deployment. No unattended flashing or formatting is provided.

For an isolated runtime checkout at
`e27d3d089086d79f06edeff4c2bd35f6e6243444`, stage this bundle's `store/` as
that checkout's `build/store/` and run:

```sh
pio run -e esp32s3 -j 1
pio run -e esp32s3 -t buildfs -j 1
```

Do not re-run the firmware repo's `scripts/build_apps.py`. That replaces
`default.elf` and the boot configuration with the heartbeat image. Do not
flash from these Garden scripts. Store mounting must remain non-formatting
on failure. Use matching firmware, bootloader, partition table, and SPIFFS
artifacts from that one build.

## Optional verified SPIFFS image

`scripts/build_relay_store.py` uses the same mkspiffs settings as the watch
clock store: size `0x4f0000`, page 256, block 4096. SPIFFS object names must
satisfy `len('/' + relative) < 32`. The script unpacks the image and compares
all 8 store files byte for byte with the deployment ZIP. It never flashes.

```sh
python3 scripts/build_relay_store.py \
  dist/relay-deployments/garden-relay-0.1.0-castle-hills-relay6.zip \
  --mkspiffs /path/to/tool-mkspiffs/mkspiffs_espressif32_arduino
```

The output is a store image only, not runtime firmware or a bootloader. Host
checks do not establish a hardware run. Encoder, e-ink, and the garden
controller app are not part of this store.
