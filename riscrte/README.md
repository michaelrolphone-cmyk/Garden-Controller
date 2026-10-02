# Garden Controller as a RiscRTE application

Target runtime reference: `michaelrolphone-cmyk/T5S3-Reader` master `7e6509d2`.

The Arduino sketches in `mcu/` are unchanged.

## Packages

| id | version | role |
| --- | --- | --- |
| relay | 0.1.0 | generic `switch.relay@1` |
| led | 0.1.0 | generic `indicator.led@1` |
| buzzer | 0.1.0 | generic `sound.buzzer@1` |
| button | 0.1.0 | generic `input.button@1` |
| garden-encoder | 0.1.1 | `input.quadrature@1`, pins A 45 and B 42 only |
| garden_controller | 0.1.5 | app policy |

`button` does not know what a press means. A profile supplies pins, active level, debounce, and long-press time. The CrowPanel knob button is GPIO 41, active-low, 30 ms debounce, 1.5 s long press. The quadrature driver no longer claims that pin.

Host check: `cc -Wall -Wextra -I riscrte/sdk -o /tmp/button-check riscrte/test/button_check.c riscrte/test/button_tu.c`

Not hardware-qualified.
