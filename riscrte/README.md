# Garden Controller as a RiscRTE application

Target runtime reference: `michaelrolphone-cmyk/T5S3-Reader` master `7e6509d2`.
App shape follows `Apps/gps.c` (`app_main`, manifest `requires`).
Driver shape follows `sdk/driver/RiscProviderV2.h` (`t5_driver_get`, ABI 2, `start` / `stop` / `quiesce`).

This package does not replace the Arduino sketches in `mcu/`. Those remain the field firmware until this ELF set is built and qualified on the boards.

## Packages

| id | version | provides | board |
| --- | --- | --- | --- |
| garden_controller | 0.1.0 | app | knob panel + relay board |
| garden-relay6 | 0.1.0 | irrigation.relay@1 | Castle Hills 6-channel board |
| garden-encoder | 0.1.0 | input.quadrature@1 | Elecrow CrowPanel 1.28 rotary |

Relay pins, active-high, from `mcu/relay/GardenSimpleRelay6Core.inc`: GPIO 1, 2, 41, 42, 45, 46. Buzzer GPIO 21, 5% duty. Relays 1-5 are zones. Relay 6 is the master valve and is not a schedule channel. Master is on while any zone or spigot run is on. Overlapping zone runs are kept.

Encoder pins from `mcu/dial/GardenKnob.ino`: A 45, B 42, button 41, pull-up, 2 edges per detent, 800 us debounce. Direction matches the field ISR (`A==B` increments). Long press is 1.5 s and is all-off.

The app never writes GPIO. Removing either driver ELF removes that capability. `quiesce` forces relay outputs off before unload.

## Host check

```
cc -Wall -Wextra -I riscrte/sdk -I riscrte/include -o /tmp/garden-riscrte-check riscrte/test/host_check.c riscrte/test/relay_tu.c riscrte/test/encoder_tu.c riscrte/include/garden_policy.c
```

Not a hardware qualification. ESP-IDF GPIO claim is compiled only with `-DGARDEN_RTE_TARGET_ESP32S3`.
