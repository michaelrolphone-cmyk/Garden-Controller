# Capability map

Runtime names come from `michaelrolphone-cmyk/RiscRTE-Drivers` and `T5S3-Reader`. This project does not invent a second name for a capability those repos already publish.

| Hardware | Capability | Provider in this branch | Other provider already written |
| --- | --- | --- | --- |
| Relay contacts | `switch.relay@1` | `relay` | none |
| Plain indicator | `indicator.led@1` | `led` | none |
| Addressable pixels | `indicator.pixel@1` | `pixel` | none; added because no runtime pixel capability exists |
| Buzzer | `sound.buzzer@1` | `buzzer` | none |
| GPIO buttons | `input.button@1` | `button` | USB HID is a different input path |
| Quadrature knob | `input.quadrature@1` | `garden-encoder` | none |
| CST816D touch | `input.touch.raw@1` | `touch` | `gt911-touch` |
| GC9A01 panel | `display.output@1` | `panel` | `display-epd-video` |
| GDEY075T7 panel | `display.output@1` | `epaper` | `display-epd-video` |
| SD card | `storage.volume@1` | `storage` | `usb-mass-storage` |
| Wi-Fi station | `net.wifi@1` | `wifi` | none; added |
| Touch bus | `i2c.bus@1` | not reimplemented | `i2c-esp32s3-v2` |
| Clock | `platform.clock@1` | not reimplemented | `platform-clock-v1` |
| Camera | `camera.capture@1` | not present; no camera on these boards | `camera-esp32s3-ov3660` |
| GNSS | `position.gnss@1` | not present; no receiver on these boards | `gps-nmea` |
| USB host | `usb.host@1` | not present | `usb-host-v2` |
| Charger | bq25896 provider | not present | `bq25896` |

`touch` requires `i2c.bus@1` and `platform.clock@1`. An app that needs a display requires `display.output@1` and can bind either `panel` or `epaper`. An app that needs storage requires `storage.volume@1` and can bind SD or USB mass storage.
