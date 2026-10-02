# Garden Controller as a RiscRTE application

Target runtime reference: `michaelrolphone-cmyk/T5S3-Reader` master `7e6509d2`.

The Arduino sketches in `mcu/` are unchanged.

## Packages

| id | version | role |
| --- | --- | --- |
| relay | 0.1.0 | generic `switch.relay@1` contact bank |
| led | 0.1.0 | generic `indicator.led@1` LED bank |
| garden-relay6 | 0.1.3 | Castle Hills relay pin profile |
| garden-encoder | 0.1.0 | `input.quadrature@1` |
| garden_controller | 0.1.3 | app policy on top of those capabilities |

`led` does not know what a channel means. A profile supplies channel count (1-8), pins, and active level. Quiesce forces every channel off. The Castle Hills indicator profile is GPIO 21, active-high. The field sketch chirps that pin; this driver exposes on, duty, and blink. The relay profile no longer claims GPIO 21.

Host check: `cc -Wall -Wextra -I riscrte/sdk -o /tmp/led-check riscrte/test/led_check.c riscrte/test/led_tu.c`

Not hardware-qualified. ESP-IDF pin claim compiles only with `-DGARDEN_RTE_TARGET_ESP32S3`.
