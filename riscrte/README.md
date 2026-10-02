# Garden Controller as a RiscRTE application

Target runtime reference: `michaelrolphone-cmyk/T5S3-Reader` master `7e6509d2`.

The Arduino sketches in `mcu/` are unchanged.

Capabilities are generic. A board profile supplies pins. Another provider can publish the same capability.

| capability | package | this board profile |
| --- | --- | --- |
| switch.relay@1 | relay | contacts 1, 2, 41, 42, 45, 46 |
| indicator.led@1 | led | plain GPIO indicator |
| indicator.pixel@1 | pixel | CrowPanel ring GPIO 48 x5, relay status GPIO 38 x1 |
| sound.buzzer@1 | buzzer | GPIO 21 |
| input.button@1 | button | knob GPIO 41, BOOT GPIO 0 |
| input.quadrature@1 | garden-encoder | A 45, B 42 |
| display.panel@1 | panel | CrowPanel 240x240, SPI 10/11/3/9/14, backlight 46 |
| input.touch@1 | touch | CrowPanel I2C 6/7 address 0x15 |
| display.epaper@1 | epaper | GDEY075T7 800x480, SPI 4/5/7/3/6, busy 2 |
| storage.block@1 | storage | paper-display SD, CS 20, MISO 19 |
| net.wifi@1 | wifi | station link, no board pins |

LCD power rails GPIO 1 and 2 are a `switch.relay` profile on the CrowPanel, not a separate capability. Not hardware-qualified.
