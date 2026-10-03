#pragma once
/* PROPOSED shared Garden/TWatch hardware mapping ABI; not implemented by Reader.
 * JSON schema: hardware/board-manifest-v1.schema.json. Normal target C ABI,
 * fixed-width scalar fields, no packing. All pointers are borrowed immutable
 * data pinned from before start through successful quiesce and stop. */
#include <stdint.h>
#define RISC_HARDWARE_DEVICE_API_V1 1u
#define RISC_HARDWARE_CATALOG_API_V1 1u
#define RISC_HARDWARE_CONFIG_API_V1 1u
#define RISC_HW_PIN_NONE (-1)
#define RISC_HW_MAX_GPIO 48u
#define RISC_HW_MAX_CHANNELS 8u
#define RISC_HW_MAX_POWER_PINS 4u
#define RISC_HW_BUS_SPI 1u
#define RISC_HW_BUS_I2C 2u
/* Exactly one matching entry is injected under hardware.device@1 into each
 * independent ELF instance. instance_id is nonzero and unique within a board.
 * compatible is vendor/model protocol identity, NOT board identity. revision
 * must be explicitly accepted in driver manifest hardware_compatibility.
 * config_type/version/size discriminate the pointed-to typed configuration.
 * Named bus dependencies must be scoped by the loader to config.bus.instance_id.
 * Multiple instances require independent ELF data/BSS and dependency contexts;
 * re-starting one singleton ELF with a second entry is expressly forbidden. */
typedef struct {
    uint32_t api_version, struct_size;
    uint64_t instance_id;
    const char *compatible;
    const char *revision;
    const char *config_type;
    uint32_t config_version, config_size;
    const void *config;
} risc_hardware_device_v1;
/* platform.board@2: owner-authorized board identity, no pin authority. */
typedef struct {
    uint32_t api_version, struct_size;
    const char *board_id;
    const char *revision;
} risc_hardware_board_identity_v2;
/* hardware.catalog@1: a board profile ELF publishes the exact bounded UTF-8
 * board manifest packaged alongside it. Loader validates JSON, maps compatible
 * entries to driver metadata, materializes typed configs and scopes dependencies.
 * Catalog does not grant access and does not run peripheral initialization. */
typedef struct {
    uint32_t api_version, struct_size;
    const char *board_id;
    const char *revision;
    const char *manifest_json;
    uint32_t manifest_size;
} risc_hardware_catalog_v1;
typedef struct {
    uint32_t struct_size;
    uint32_t kind;
    uint64_t instance_id;
    /* Preserve declared controller number; board-port JSON controller_namespace
     * and optional physical_controller select the scoped runtime bus owner. */
    uint32_t controller;
    uint32_t frequency_hz;
    uint8_t mode;
    uint8_t reserved[3];
    int16_t sclk, mosi, miso, sda, scl;
} risc_hw_bus_v1;
/* config_type = gpio.bank; covers discrete contacts, indicators, pulse sounders
 * and buttons. No board-specific interpretation of channel index. */
typedef struct {
    uint32_t struct_size;
    uint8_t count, active_high, pull_up, reserved;
    int16_t pins[RISC_HW_MAX_CHANNELS];
    uint32_t debounce_us, long_press_us, click_min_us;
} risc_hw_gpio_bank_v1;
/* config_type = input.quadrature; related button comes from the explicitly
 * selected input.button dependency instance, never "first installed button". */
typedef struct {
    uint32_t struct_size;
    int16_t a, b;
    uint8_t pull_up, edges_per_detent;
    int8_t direction;
    uint8_t reserved;
    uint32_t debounce_us;
    uint64_t button_instance_id;
} risc_hw_quadrature_v1;
/* config_type = pixel.ws2812; order=0 GRB, 1 RGB. Protocol timing is in ELF. */
typedef struct {
    uint32_t struct_size;
    int16_t pin;
    uint8_t count, order;
} risc_hw_pixel_v1;
/* config_type = display.spi; controller-specific dimensions must be admitted
 * by that chip driver. reset=-1 means physically absent: both reset delays
 * must be zero; present reset requires each delay in 1..500ms. Chip drivers
 * may reject absence. -1 also means absent optional backlight/busy/power pin. */
typedef struct {
    uint32_t struct_size;
    risc_hw_bus_v1 bus;
    uint16_t width, height;
    uint16_t offset_x, offset_y;
    uint8_t rotation, reserved[3];
    int16_t cs, dc, reset, backlight, busy;
    uint8_t reset_active_high, busy_active_high, backlight_active_high, power_count;
    int16_t power_pins[RISC_HW_MAX_POWER_PINS];
    uint8_t power_active_high[RISC_HW_MAX_POWER_PINS];
    uint32_t reset_assert_ms, reset_recovery_ms;
} risc_hw_spi_display_v1;
/* config_type = touch.i2c; reset absence/timing semantics match display.spi. */
typedef struct {
    uint32_t struct_size;
    risc_hw_bus_v1 bus;
    uint16_t width, height;
    uint8_t address, reset_active_high, irq_active_high, irq_pull_up;
    int16_t reset, irq;
    uint32_t reset_assert_ms, reset_recovery_ms;
} risc_hw_i2c_touch_v1;
/* config_type = storage.sd-spi; detect/protect may be absent (-1). */
typedef struct {
    uint32_t struct_size;
    risc_hw_bus_v1 bus;
    int16_t cs, detect, write_protect;
    uint8_t detect_active_high, write_protect_active_high;
} risc_hw_sd_spi_v1;
/* config_type = radio.integrated; features bit0 station, bit1 soft AP. */
typedef struct {
    uint32_t struct_size, unit, features;
} risc_hw_radio_v1;

/* Additional shared types for TWatch. They do not imply Garden implements the
 * corresponding device providers. IRQ edge:0 none,1 rising,2 falling,3 both. */
typedef struct {
    int16_t pin;
    uint8_t active_high, initial_high, pull_up, irq_edge;
} risc_hw_gpio_role_v1;
typedef struct {
    uint64_t instance_id;
    uint32_t minimum_uv, maximum_uv, requested_uv;
} risc_hw_power_rail_v1;
/* config_type = sensor.imu-i2c; reject observations outside allowed_chip_ids.
 * IDs come from an explicitly selected physical variant, never auto-selection. */
typedef struct {
    uint32_t struct_size;
    risc_hw_bus_v1 bus;
    uint8_t address, allowed_chip_id_count;
    uint16_t allowed_chip_ids[8];
    risc_hw_gpio_role_v1 reset, irq;
    risc_hw_power_rail_v1 power;
} risc_hw_imu_i2c_v1;
/* config_type = radio.transceiver-spi; model is the envelope compatible ID. */
typedef struct {
    uint32_t struct_size;
    risc_hw_bus_v1 bus;
    int16_t cs;
    risc_hw_gpio_role_v1 reset, irq, busy;
    risc_hw_power_rail_v1 power;
    uint32_t authorized_frequency_min_hz, authorized_frequency_max_hz;
    uint8_t tcxo_code;
    uint8_t reserved[3];
} risc_hw_radio_spi_v1;
/* config_type = audio.i2s-port; bus dependency is scoped to bus_instance_id.
 * direction:1 TX,2 RX; pdm:0 standard I2S,1 PDM. */
typedef struct {
    uint32_t struct_size, controller;
    uint64_t bus_instance_id;
    int16_t bclk, ws, data;
    uint8_t direction, pdm;
    uint32_t sample_rate_hz;
    risc_hw_gpio_role_v1 enable;
    risc_hw_power_rail_v1 power;
} risc_hw_i2s_v1;
/* config_type = output.ir; carrier generated by an owned PWM/timing provider. */
typedef struct {
    uint32_t struct_size;
    risc_hw_gpio_role_v1 output;
    uint32_t carrier_hz;
    uint16_t duty_per_mille;
} risc_hw_ir_v1;
