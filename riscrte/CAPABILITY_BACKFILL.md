# Garden ELF driver contracts and runtime backfill

This change implements device behavior in Garden ELFs. It does **not** supply the
missing runtime/CPU-port capabilities, qualify hardware, or port the Garden apps.
Missing, duplicate, truncated, wrong-version or unauthorized dependencies reject
`start`. A built ELF is not evidence that Reader can activate it today.

Baseline: Garden `f6cb516a7f9d3820d01cfc80e21ee88b34e9a61a`.
Authoritative read-only Reader snapshot:
`3d9bc4f373679f5ae8dd184db6a8d0afa5a40231`.
The seven `Risc*.h` files in [sdk](sdk) are byte-for-byte copies from that snapshot.
The architecture is **RiscRTE on Xtensa ESP32-S3**, not a RISC-V ISA port.

## Original firmware coverage

| Original hardware-facing implementation | Board and pins | Garden ELF / contract | Host fixture / remaining runtime work |
| --- | --- | --- | --- |
| [relay GPIO bank](../mcu/relay/GardenSimpleRelay6Core.inc) and GardenSimpleRelay6.ino | Castle Hills: 1,2,41,42,45,46; active high | `relay` / `switch.relay@1` | mask, bounds, safe-off/write failure, partial admission; GPIO owner |
| Same source: buzzer pulse train | GPIO21 | `buzzer` / `sound.buzzer@1` | exact pulse durations, bounded rejection; GPIO waveform |
| Same source: Adafruit_NeoPixel status | GPIO38, one GRB/800kHz pixel | `pixel` / `indicator.pixel@1` | waveform length, scaling; timed GPIO waveform |
| [dial quadrature ISR](../mcu/dial/GardenKnob.ino) | A45, B42, two A edges/detent, 800us debounce | `garden-encoder` / `input.quadrature@1` | signed detents and button forwarding; caller polling cadence |
| Dial button input | GPIO41, active low/pullup | `button` / `input.button@1` | debounce, click consume; GPIO input |
| Dial power indicator | GPIO40 | `led` / `indicator.led@1` | duty and range; GPIO/PWM |
| Dial pixel ring | GPIO48, five GRB/800kHz pixels | `pixel` / `indicator.pixel@1` | frame bounds, brightness, wire count; waveform |
| Dial Arduino_GC9A01 / Arduino_ESP32SPI | 240x240; SCLK10/MOSI11/CS9/DC3/reset14/backlight46; power enables1,2 | `panel` / canonical `display.output@1` | all 115200 bytes, first/last distinct pixels, native RGB565 to MSB-first wire order, frame/token lifetime; SPI + GPIO + provider polling |
| Dial CST816D readTouch/initTouch | SDA6/SCL7/address0x15/IRQ5/reset13; 240x240 | `touch` / canonical `input.touch.raw@1` | current snapshot, DOWN/MOVE/UP, independent subscriber queues/GAP/stale token; correctly configured canonical I2C owner |
| [paper GxEPD2_750_GDEY075T7](../mcu/paperdisplay/GardenEInkDisplay.ino) | 800x480; MOSI4/SCLK5/CS7/DC3/reset6/BUSY2 active low | `epaper` / canonical `display.output@1` | all 48000 bytes and MONO1 inversion, refresh completion; SPI + GPIO + polling |
| Paper SD wiring declaration | CS20/MISO19; shared MOSI4/SCLK5 | `storage` / canonical `storage.volume@1` | actual SDHC command/CRC fixture plus FAT16 read/create/write/commit/abort/remove; shared SPI owner. Original paper sketch declares SD pins but does not call SD APIs |
| WiFi on all three firmware trees | ESP32-S3 integrated radio; station and captive-portal AP | `wifi` / `net.wifi@1` (append-only AP/address suffix) | joining/up distinction, AP password policy, address byte order; CPU-port radio service |
| Board selection (previous garden-relay6 was only a header) | Relay=1, dial=2, paper=3 | `garden-relay6`, `garden-crowpanel`, `garden-paper` / `board.garden@1` | wrong-board rejection; platform board identity |

`mcu/relay/GardenEInkZoneDisplay.ino` is retained; it is not a fourth independent
physical controller target. The inventory includes the three actual firmware
hardware trees, not just the original migration package list.

HTTP/HTTPS, DNS lookup/server, WebServer, Preferences, wall-clock/NTP, JSON,
rendering/fonts, schedules, master/slave synchronization and Serial diagnostics
are application/OS services used by the old firmware. They are not a new physical
Garden driver. Their app integration remains separate; this PR does not claim a
working captive portal, network stack, persistence service or full Garden app.
No default.elf/PaperSpace launcher is implemented. Existing demo apps are **not
safe hardware tests**; in particular do not run the zone-start demo on a device.

## ABI source of truth

Consumers must compile against [RiscDisplayOutputV1.h](sdk/RiscDisplayOutputV1.h),
[RiscStorageVolumeV1.h](sdk/RiscStorageVolumeV1.h),
[RiscTouchV1.h](sdk/RiscTouchV1.h),
[RiscI2cBusV1.h](sdk/RiscI2cBusV1.h), and
[RiscPlatformClockV1.h](sdk/RiscPlatformClockV1.h).
The old PanelApi/EpaperApi/StorageApi/TouchApi table names now alias these exact
canonical types; they no longer describe competing tables under the same ID.
Board profile types remain available for source/reference use.

Provider root is [RiscProviderV2.h](sdk/RiscProviderV2.h): ABI2 descriptor, dependency
array, `start`, `stop`, `quiesce`; display descriptors include the existing
`risc_driver_poll_v2` suffix. Core invokes `poll(budget_ms)` on its provider-owner
executor. No special panel callback or firmware device proxy is required.

New low-level contracts are defined in full in
[GardenPlatformV1.h](sdk/GardenPlatformV1.h). These are **proposals with implemented
consumers**, not existing Reader APIs. Do not substitute any other SPI/GPIO/radio
table just because its capability name is similar. Upstream must adopt this exact
v1 layout or coordinate a versioned adapter before enabling these packages.

All tables use ordinary C target ABI, without packing: leading `uint32_t
api_version=1`, `uint32_t struct_size=sizeof(table)`, then context and function
pointers in header order (except the board table, whose third field is `uint32_t
board`). ESP32-S3 pointers are 32 bits, context offset8, first callback offset12;
board table is 12 bytes. Token arguments are unsigned 64 bits, zero invalid.
Host fixture pointer sizes differ intentionally; host tests are behavioral, target
compilation is the layout/build check. The SDK header definitions, not a host
serialized struct, define binary layout. Append-only larger tables are admitted;
wrong API versions or smaller tables are rejected. At most16 dependencies are
accepted, matched by string/version, with duplicate matching IDs rejected.

## Exact backfill interfaces

| Capability | Type / members after version and size | Required implementation semantics |
| --- | --- | --- |
| `platform.board@1` | `garden_board_v1`: board enum | Read-only actual board selection authorized by the CPU/board port. Never guess from app name. One of1/2/3; board profile ELF only starts for its exact board |
| `board.garden@1` | same `garden_board_v1` | Implemented here by three separate profile ELFs. Runtime selects only the profile matching actual platform.board; there is no pin authority in a manifest alone |
| `platform.gpio@1` | `garden_gpio_v1`: context; claim(pin,output,initial,pullup,&token); write(token,level); read(token,&level); pwm(token,hz,duty,maximum); release(token); waveform(token,durations_ns,count) | Global pin arbitration across GPIO, SPI, I2C, PWM and timing peripherals. Output latch must be set before enabling output. Denied claims have no effect and no token. read/write/PWM must be bounded; false reports error. PWM accepts duty0..maximum and changes output atomically. waveform alternates HIGH/LOW starting HIGH, at most768 durations and20ms, completes synchronously with no retained caller buffer, ends LOW. No device-specific WS2812/buzzer logic here |
| `spi.bus@1` | `garden_spi_v1`: context; claim(sclk,mosi,miso,cs,&token); begin(token,hz,mode,timeout_ms); exchange(token,tx,rx,length); end(token); idle_clocks(token,hz,clocks); release(token) | Controller-owner ELF. Same bus pins may be shared by distinct CS claims; conflicting pin/controller assignments and duplicate CS fail. begin serializes the entire transaction, asserts CS and establishes a total deadline; exchanges cannot reset it. mode0, MSB first; <=512 bytes/exchange. NULL tx sends0xff, NULL rx discards. end deasserts CS/drains I/O. idle_clocks arbitrates the controller with every CS HIGH. False end/release retains ownership and requires retry |
| `platform.radio@1` | `garden_radio_v1`: context; claim; join; state; leave; release; start_ap; stop_ap; addresses | One exclusive radio-stack session. Strings copied before return; no retained password pointers. join accepts a request, state returns DOWN0/JOINING1/UP2 plus dBm. AP+station coexist. start_ap takes three IPv4 byte[4] arrays; addresses writes two12-byte address/gateway/netmask groups. stop_ap/leave drain within100ms, release only after stopped; failures retain token. This belongs in a CPU-port radio provider, not GPIO or Garden device policy |
| `i2c.bus@1` | canonical `risc_i2c_bus_api_v1` | Existing ABI; backfill board configuration and ownership for dial SDA6/SCL7. Claim address0x15, atomic write-register/read with repeated START, total20ms timeout; no Wire import in touch ELF |
| `platform.clock@1` | canonical `risc_platform_clock_api_v1` | Existing ABI; monotonic milliseconds, sleep_ms genuinely yields. Required for bounded waits and pulse/reset timing |

Raw GPIO methods except waveform must complete within1ms; the radio methods
except drain must complete within20ms. Long radio work is asynchronous inside its
provider. SPI begin's timeout includes lock admission and the entire held
transaction; exchange/end must terminate within that same budget even if the
caller has yielded. No provider may retain client buffers after synchronous
return. No tokens may be reused while the provider generation remains mapped.
Core pins dependency tables for the whole consumer lifetime, including failed
activation until quiesce succeeds.

## Device behavior and lifecycle

GPIO consumers start only after board and raw dependencies validate. Relay pins
start OFF; buzzer/pixel/LED start dark/silent. The dial indicator is GPIO40, not
WS2812 data GPIO48; relay GPIO21 is a buzzer, not an LED. quiesce turns outputs off,
then releases only owned pins. Failed safe-off or failed release returns false
and retains claims for retry. stop is idempotent and cannot override failed
quiescence. Legacy private bind functions in the simple GPIO sources remain for
source compatibility; production start replaces them with authorized dependency
tables and the fixed board profile. The loader never needs to call them.

The generic relay's legacy optional `chirp` returns false: relay owns no sounder;
use `sound.buzzer`. Encoder's legacy button methods now forward to its declared
`input.button@1` dependency, instead of always returning false. Buttons debounce
at30ms, suppress click after1.5s long press, and consume pending events. Caller
polling must be frequent enough to observe real edges; no GPIO ISR integration is
claimed. Detent count saturates at int32 limits. Simple GPIO/pixel/radio APIs and
filesystem operations require serialized calls by their grant-owning executor;
concurrent unsynchronized callers are not supported by those Garden v1 tables.

LED set_duty uses real1kHz PWM. LED blink and buzzer pattern use actual raw pulse
durations, at most20ms total per call; longer patterns reject and should be
scheduled as multiple app operations. WS2812 output is GRB,800kHz timing encoded
in this ELF, with1ms low latch. Brightness scales a retained RGB frame and does
not destroy its unscaled values. Quiesce sends black and retains the claim on
transmission/release failure.

Displays provide one retained frame and one latest present token. Acquire grants
exclusive mutable pixels; submit consumes that lease; release abandons an
unsubmitted frame. Frame/present serials do not reset on restart. Full-frame
refresh is supported; valid damage rectangles are accepted but conservatively
expanded to the whole frame. Only FIFO is accepted; mailbox is not advertised.
Concurrent status/poll paths use a nonblocking atomic state lock. The holder must
serialize its frame acquire/release/submit calls. Nonblocking submit performs no
SPI I/O. Poll sends one row (480 RGB bytes or100 MONO bytes), checks errors, then
returns. Panel rows share a single raw transaction. E-paper uses separate
cooperative power-on/refresh/power-off BUSY phases; a15s phase timeout reports
FAILED. Low is BUSY; data is inverted because canonical MONO1 means1=black.
Reset terminates failed paper refresh before releasing resources. Held frames or
active transfers block quiescence; revocation prevents new acquisitions and
submissions while existing work drains. wait_present clamps to250ms and yields;
provider-owner polling must continue independently. Backlight uses PWM; e-paper
brightness is explicitly unsupported, as indicated by absent brightness flag.

Touch owns address0x15, reset13 and IRQ5. It uses the report format already in the
original dial source, rather than guessing additional controller-ID admission
rules. Start resets and reads the initial report. One bounded report per poll;
up to4 subscribers,32 events each, monotonic subscription generations, explicit
GAP on overflow or invalid report. Snapshot is authoritative. Transient read
failure preserves previous state/queued events. All report and queue updates are
serialized by an atomic try-lock; contention returns failure, never spins. Live
subscriptions block quiescence, and failed I2C release retains its claim.

Storage implements SD SPI initialization (CMD0/8/55+41/58/16/9), SDSC byte versus
SDHC sector addressing, CMD17/24 token exchange, command CRC7 and data CRC16.
Reads never use the old five-byte command as a512-byte transmit buffer. It owns
FAT16/FAT32 parsing, paths, LFN/short names, one directory and one file handle,
exclusive create, partial reads/writes, commit, abort and remove. FAT code was
adapted from Reader usb_mass_storage at the pinned snapshot, retaining useful
filesystem behavior while replacing USB/SCSI transport. Geometry now checks FAT
capacity and fresh root bounds; MBR entries survive scratch-buffer BPB probes.
External paths are bounded to511 bytes, directory scans8192 entries, individual
read/write results4096 bytes maximum, and operations stop after8192 sector calls
or2s with scheduler yields every8 sector calls. No FAT12/exFAT support is claimed.
ready is separate from refresh's service success, matching the canonical ABI.
No media/error is reported through ready/last_error, not fabricated success.
Open handles block teardown. A failed media write may require filesystem recovery;
this is not a journaled, power-loss-atomic filesystem. Callers must inspect short
I/O and close results. No fixture proves all corrupt-media or power-cut cases.

## Package and validation evidence

Changed existing package versions: relay/buzzer/button/led/pixel/wifi
0.1.0→0.1.1; garden-encoder/panel/epaper/storage/touch0.1.1→0.1.2;
garden-relay6 0.1.3→0.1.4. New board packages garden-crowpanel and garden-paper are
0.1.0. No remote `driver-*` tags were present when checked; no release was made.
App versions and app behavior are unchanged.

Build discovery uses source manifest directories, preserving `garden_encoder`
directory and `garden-encoder` package identity. There are14 driver/profile ELFs;
none can be silently skipped. Policy source path is corrected. Direct ESP-IDF
includes are no longer needed in these portable driver ELFs: GPIO access is a
capability dependency, not an unresolved gpio_config import. Driver link output
rejects unexpected undefined imports; only the small libc import allowlist is
accepted. No peripheral/firmware symbols appear in the driver ELFs.

Run from repository root:

```
python3 scripts/test_contracts.py
python3 scripts/build_packages.py --drivers-only
python3 scripts/check_packages.py
```

Local results:14 host fixture executables passed with UndefinedBehaviorSanitizer;
14 ESP32-S3 ELF builds passed using xtensa-esp32s3-elf-gcc8.4.0;14 archive/catalog
identity, dependency, metadata and payload/hash checks passed. Fixtures exercise
missing/truncated dependencies, wrong boards, partial GPIO admission, failed
release/retry, duplicate starts/stops, plus device-specific behavior above.
The mock SDHC card parses transmitted commands and checks CRC against an
independent reference; a FAT16 image exercises real filesystem bytes.
AddressSanitizer startup stalled in this macOS sandbox and was interrupted;
no ASan pass is claimed. UBSan and ordinary host execution completed.

The older direct-bind test translation units remain as historical source tests;
`scripts/test_contracts.py` is the supported dependency/lifecycle suite for this
ABI. No test, build, or script in this work opens a serial port, flashes a board,
runs a relay app, or claims hardware qualification. Local target compilation is
not CI or runtime-loading evidence.

## Integration prerequisites before on-device use

1. Implement/adopt the four new low-level runtime contracts above, with exact
   layout/version checking, authorized platform board identity and cross-provider
   resource arbitration. Raw controller implementations must live in their
   platform/bus providers; core remains a capability/lifetime manager.
2. Configure the canonical I2C provider for dial pins and install the right board
   profile. Paper display and SD must share one SPI controller owner and distinct
   CS leases. Remove conflicting original firmware owners before admission.
3. Confirm libc exports and loader support for the canonical polling suffix;
   continue polling already-active displays during revocation until quiescent.
   Keep failed-start and failed-quiesce ELFs/dependencies mapped for cleanup.
4. Provide sufficient ELF data memory for the115200-byte panel or48000-byte paper
   buffer. Validate timing, panel initialization, electrical polarity, actual
   controller revisions, SD media compatibility and radio behavior on hardware
   in a separately authorized session. Host mocks intentionally cannot prove any
   of those facts.
5. Port full Garden application services separately. Driver compatibility does
   not complete schedules, web UI, networking/persistence, default.elf selection
   or a PaperSpace launcher.
