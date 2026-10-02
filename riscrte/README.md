# Garden Controller as a RiscRTE application

Target runtime reference: `michaelrolphone-cmyk/T5S3-Reader` master `7e6509d2`.

The Arduino sketches in `mcu/` are unchanged.

## Packages

| id | version | role |
| --- | --- | --- |
| relay | 0.1.0 | generic `switch.relay@1` contact bank |
| led | 0.1.0 | generic `indicator.led@1` LED bank |
| buzzer | 0.1.0 | generic `sound.buzzer@1` sounder |
| garden-relay6 | 0.1.3 | Castle Hills relay pin profile |
| garden-encoder | 0.1.0 | `input.quadrature@1` |
| garden_controller | 0.1.4 | app policy on top of those capabilities |

`buzzer` does not know why a channel sounds. A profile supplies pins and active level. Quiesce forces every channel off. The Castle Hills buzzer profile is GPIO 21, active-high. Chirp is the field pattern: 50 us on, 950 us off, 8 pulses. The LED driver no longer claims that pin. The garden app does not require `indicator.led`.

Host check: `cc -Wall -Wextra -I riscrte/sdk -o /tmp/buzzer-check riscrte/test/buzzer_check.c riscrte/test/buzzer_tu.c`

Not hardware-qualified. ESP-IDF pin claim compiles only with `-DGARDEN_RTE_TARGET_ESP32S3`.
