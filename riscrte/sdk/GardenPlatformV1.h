#pragma once
/* Proposed Garden contracts; NOT provided by the current Reader runtime.
 * All layouts use the target C ABI. See CAPABILITY_BACKFILL.md. */
#include "RiscProviderV2.h"
#define GARDEN_PLATFORM_API_V1 1u
#include "RiscHardwareConfigV1.h"
/* GPIO claims are exclusive across GPIO/PWM/SPI/I2C/waveform providers.
 * claim sets the initial output latch BEFORE enabling output. False has no
 * effect. Tokens are never reused; release false retains ownership. */
typedef struct {
    uint32_t api_version, struct_size; void *context;
    bool (*claim)(void *, uint8_t pin, bool output, bool initial, bool pullup, uint64_t *token);
    /* Cancels PWM on this token before forcing a static level. */
    bool (*write)(void *, uint64_t token, bool level);
    bool (*read)(void *, uint64_t token, bool *level);
    bool (*pwm)(void *, uint64_t token, uint32_t hz, uint16_t duty, uint16_t maximum);
    bool (*release)(void *, uint64_t token);
    /* Raw pulse durations, alternating high/low, nanoseconds, maximum 768
     * durations; zero durations skip a level without a pulse. Retains the final phase level (even count LOW, odd HIGH).
     * Synchronous, bounded 20ms, no retained pointer. */
    bool (*waveform)(void *, uint64_t token, const uint32_t *durations_ns, size_t count);
} garden_gpio_v1;
/* SPI bus owner claims controller/pins and arbitrates complete transactions.
 * begin/end hold CS across multiple exchanges; begin has total timeout budget.
 * exchange NULL tx sends 0xff; NULL rx discards; max 512 bytes per exchange.
 * idle_clocks runs with all chip selects HIGH (SD initialization).
 * GPIO DC is controlled by its separate claim while SPI is held.
 * Native bus provider must reserve SCLK/MOSI/MISO centrally. */
typedef struct {
    uint32_t api_version, struct_size; void *context;
    bool (*claim)(void *, uint8_t sclk, uint8_t mosi, int8_t miso, uint8_t cs, uint64_t *token);
    bool (*begin)(void *, uint64_t token, uint32_t hz, uint8_t mode, uint32_t timeout_ms);
    bool (*exchange)(void *, uint64_t token, const uint8_t *tx, uint8_t *rx, size_t length);
    bool (*end)(void *, uint64_t token);
    bool (*idle_clocks)(void *, uint64_t token, uint32_t hz, uint16_t clocks);
    bool (*release)(void *, uint64_t token);
} garden_spi_v1;
/* CPU-port radio service. Station and AP may coexist. Strings copied on join; completion is queried
 * with state, never inferred from join acceptance. Credentials max 63 bytes.
 * Exclusive token arbitrates the radio; leave drains operations within 100ms. */
typedef struct {
    uint32_t api_version, struct_size; void *context;
    bool (*claim)(void *, uint64_t *token);
    bool (*join)(void *, uint64_t token, const char *ssid, const char *password);
    bool (*state)(void *, uint64_t token, uint8_t *state, int8_t *rssi);
    bool (*leave)(void *, uint64_t token);
    bool (*release)(void *, uint64_t token);
    bool (*start_ap)(void *, uint64_t token, const char *ssid, const char *password,
                     const uint8_t address[4], const uint8_t gateway[4], const uint8_t netmask[4]);
    bool (*stop_ap)(void *, uint64_t token);
    bool (*addresses)(void *, uint64_t token, uint8_t station[12], uint8_t access_point[12]);
} garden_radio_v1;
