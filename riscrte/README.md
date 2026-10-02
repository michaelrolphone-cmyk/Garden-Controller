# Garden Controller packages

Two installable apps, one per board:

- `garden-relay` requires `switch.relay@1` and `sound.buzzer@1`.
- `garden-encoder` requires `input.quadrature@1` and `input.button@1`.

Build:

```
python -m pip install platformio==6.1.19
pio pkg install --global --tool espressif/toolchain-xtensa-esp32s3@8.4.0+2021r2-patch5
git clone --depth 1 https://github.com/michaelrolphone-cmyk/T5S3-Reader.git third_party/T5S3-Reader
python scripts/build_packages.py
```

Release tags match T5S3-Reader: `app-garden-relay-v0.1.0`, `app-garden-encoder-v0.1.0`, `driver-relay-v0.1.0`. Assets are `application-{id}-{version}-xtensa-esp32s3.rte.zip` and `driver-{id}-{version}-xtensa-esp32s3.rte.zip`. The workflow updates `release-index.json` on the `release-index` branch. Point the app store and driver manager at `michaelrolphone-cmyk/Garden-Controller`.

Not hardware-qualified. The workflow produces the ELFs; this branch does not contain built packages.
