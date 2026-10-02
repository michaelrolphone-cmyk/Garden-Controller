# Garden Controller as a RiscRTE application

Capability names match `RiscRTE-Drivers` where that repo already has a provider. See `CAPABILITIES.md`.

`touch` publishes `input.touch.raw@1` and requires `i2c.bus@1` plus `platform.clock@1`. `panel` and `epaper` both publish `display.output@1`. `storage` publishes `storage.volume@1`.

Added because the runtime has no provider: `switch.relay`, `indicator.led`, `indicator.pixel`, `sound.buzzer`, `input.button`, `input.quadrature`, `net.wifi`.

Not present because these boards have no such hardware: `camera.capture`, `position.gnss`, `usb.host`.

Not hardware-qualified. Panel and e-paper still do not send the Arduino init sequences.
