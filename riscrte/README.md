# Garden Controller RiscRTE app

`garden_controller` 0.2.0 is an installable app. The firmware calls `app_main`. The app calls `t5_app_get_api` and `t5_provider_capability_get_api()->acquire`, the same entry used by `Apps/camera_utility.c` in T5S3-Reader. There is no `garden_app_bind`.

Required installed drivers: `switch.relay@1`, `sound.buzzer@1`, `input.quadrature@1`, `input.button@1`. Each driver claims its field pins inside `start`. The loader does not bind a port.

This tree is source. It is not a built ELF and it is not hardware-qualified. Build with the T5S3-Reader native app toolchain and `-DGARDEN_RTE_TARGET_ESP32S3` for the driver GPIO claim.
