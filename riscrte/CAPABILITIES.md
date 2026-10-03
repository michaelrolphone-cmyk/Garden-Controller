# Capability map

The maintained driver inventory, canonical ABI mappings, exact proposed runtime
interfaces and board pin matrix are in [CAPABILITY_BACKFILL.md](CAPABILITY_BACKFILL.md).

`display.output@1`, `storage.volume@1`, `input.touch.raw@1`, `i2c.bus@1`,
`platform.clock@1` and the provider ABI use Reader's pinned SDK definitions.
`GardenPlatformV1.h` contains the explicitly proposed low-level contracts needed
for activation; their presence here does not mean Reader implements them.
