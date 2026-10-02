# Garden Controller as a RiscRTE application

Target runtime reference: `michaelrolphone-cmyk/T5S3-Reader` master `7e6509d2`.

The Arduino sketches in `mcu/` are unchanged.

## Packages

| id | version | role |
| --- | --- | --- |
| relay | 0.1.0 | generic `switch.relay@1` contact bank |
| garden-relay6 | 0.1.2 | Castle Hills board profile only |
| garden-encoder | 0.1.0 | `input.quadrature@1` |
| garden_controller | 0.1.2 | app policy on top of those capabilities |

`relay` does not know what a channel switches. A profile supplies channel count (1-8), pins, active level, and an optional indicator pin. Quiesce opens every contact. The Castle Hills profile is GPIO 1, 2, 41, 42, 45, 46, active-high, indicator GPIO 21. The garden app maps channels 1-5 to zones and channel 6 to master-follow. Another app can bind a different profile.

Host check: `cc -Wall -Wextra -I riscrte/sdk -o /tmp/relay-check riscrte/test/relay_check.c riscrte/test/relay_tu.c`

Not hardware-qualified. ESP-IDF pin claim compiles only with `-DGARDEN_RTE_TARGET_ESP32S3`.
