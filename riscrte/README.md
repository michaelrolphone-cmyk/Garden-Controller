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

Field pins:
- Relay contacts: GPIO 1, 2, 41, 42, 45, 46, active-high.
- Relay status LED: GPIO 38. CrowPanel LED ring: GPIO 48. Neither is the buzzer.
- Buzzer: GPIO 21, active-high, 5% chirp.
- Knob button: GPIO 41, active-low. BOOT button: GPIO 0, active-low.

Not hardware-qualified.
