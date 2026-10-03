# Shared Garden/TWatch hardware mapping proposal v1

Header: `riscrte/sdk/RiscHardwareConfigV1.h` (copy exactly between projects).
Capability injected by future loader: **hardware.device@1**, immutable
`risc_hardware_device_v1`, one physical entry per independently mapped ELF instance.
No board ID or package name may substitute for chip compatibility.

Board JSON schema id: `riscrte.board-hardware`, schema_version:1. Top level:
`board_id`, `revision`, `buses`, `devices`. Bus records have numeric `instance_id`,
`kind` (`spi`/`i2c`), `controller`, `frequency_hz`, `mode`, and a `pins` object.
Device records have numeric unique `instance_id`, `chip` (`vendor`, `model`,
`revision`), `compatible` (one exact vendor/protocol ID), `config_type`,
`config_version`:1, `config` (typed fields below), and optional `bindings` mapping
capability ID to the exact related device instance ID. Bus configs reference a
bus by numeric `bus_instance_id`; loader expands its complete typed bus record.
Every pin is an integer 0..48 or -1 only where explicitly optional; missing
required pins are errors, never defaults. Booleans become uint8_t 0/1. Reserved
fields are zero. struct_size is generated from the target header, never JSON.

Driver manifest extension **hardware_compatibility** is an array of exact
`{compatible, revisions, config_type, config_version}` records. Revisions are
explicit string lists; `unspecified` means the original source did not identify
silicon revision, not arbitrary autodetection. No wildcard revisions. Drivers
repeat compatibility/config validation before any resource claim. Unknown chips,
unsupported revision/schema, impossible pins or bus overlap fail without I/O.
Future Driver Manager filtering may use this metadata; it is not implemented.

Initial shared config types are gpio.bank, input.quadrature, pixel.ws2812,
display.spi, touch.i2c, storage.sd-spi, radio.integrated. Exact fields/layouts are
in the header. TWatch may add additional versioned typed structs/config_type IDs
without altering this envelope; do not reinterpret an existing type/version.

Multiple device instances use distinct ELF data/BSS and scoped dependency tables.
SPI/I2C dependency context must match bus.instance_id; related input.button or
sound.buzzer dependencies must match explicit bindings, never registry ordering.
Raw providers arbitrate controllers/pins and reject conflicts across instances.
Mapping/config injection, independent instantiation, graph scoping and hardware
resource policy remain Reader backfill, not Garden/TWatch firmware code.

Board profile ELFs publish hardware.catalog@1 (bounded JSON plus board identity),
requiring platform.board@2 to match their board_id and revision. All descriptor
and config pointers stay pinned through failed-start cleanup and quiescence.


## Type registry and extension convention

A (config_type, config_version) pair names exactly one C layout and semantic
contract across projects. A new layout uses a new name/version, never a parallel
struct with the same discriminator. The envelope and bus record remain unchanged.
The shared header also reserves sensor.imu-i2c, radio.transceiver-spi,
audio.i2s-port and output.ir layouts for richer consumers; Garden has no drivers
for those types. These include chip IDs, IRQ roles, rail voltage constraints,
authorized RF band/TCXO, I2S/PDM directions and IR carrier/duty. They do not grant
hardware authorization by their presence.

T-Watch's additive TWatchHardwareV1.h owns controller.i2c, peripheral.i2c,
power.axp2101, audio.i2s and radio.lora v1 layouts. Its audio.i2s is distinct from
the shared audio.i2s-port record; consumers must never cast between them. Its
nested power.axp2101 device.struct_size must cover the whole selected config.
No changes to T-Watch files are made here. The Garden JSON schema intentionally
validates the seven Garden config types; extension projects supply explicit
schemas for their registered additional types using the same envelope/bus/binding
rules. Unknown types are rejected until that schema and exact typed materializer
are installed. A mapper must not use sizeof or memcpy from an unrelated layout.

## Errors, ownership and bounds

All configuration is immutable and borrowed. Catalog strings/JSON and injected
config records remain mapped through failed-start cleanup, successful quiesce
and stop. JSON maximums are8 buses,64 devices,96 bytes per identity string;
instance IDs in JSON are1..2147483647 (ABI reserves uint64). UTF-8 identities
must have no embedded NUL. Config arrays use fixed bounds from the header. JSON
booleans convert to uint8 0/1, no coercion of strings or out-of-range integers.
Missing/duplicate/unknown fields fail schema validation. Scope and authorization
are established before start, then chip drivers validate again before I/O.
Admission returns false for unavailable providers or unsupported hardware. Runtime
must retain dependencies/ELF after partial admission until quiesce succeeds.

One executor serializes each ordinary provider's calls; display/touch implement
specific nonblocking locks described in CAPABILITY_BACKFILL.md. Cross-instance
SPI transactions and shared bus resources are arbitrated by the raw owner; the
mapper cannot authorize the same GPIO as both a discrete output and a bus signal.
I2C devices may share a bus with distinct addresses; SPI devices share controller
signals with distinct CS. Configuration and scoped context must agree, otherwise
raw claim fails. A future Driver Manager may filter by compatibility metadata,
but filtering never replaces runtime validation or authorization.
