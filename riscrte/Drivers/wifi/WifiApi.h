#pragma once
/* net.wifi@1. Generic station link. Radio comes from a provider. */
#include <stdbool.h>
#include <stdint.h>
#define WIFI_API_V1 1u
typedef enum { WIFI_LINK_DOWN = 0, WIFI_LINK_JOINING, WIFI_LINK_UP } wifi_link_t;
typedef struct {
    uint8_t address[4], gateway[4], netmask[4];
} wifi_ipv4_v1;
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    bool (*connect)(void *context, const char *ssid, const char *password);
    void (*disconnect)(void *context);
    wifi_link_t (*status)(void *context);
    int8_t (*rssi)(void *context);
    bool (*start_ap)(void *context, const char *ssid, const char *password, const wifi_ipv4_v1 *config);
    bool (*stop_ap)(void *context);
    bool (*addresses)(void *context, wifi_ipv4_v1 *station, wifi_ipv4_v1 *access_point);
} wifi_api_v1;

/* The append-only v1 extension is declared in the structure above; IPv4 values
 * are byte arrays in network order. SSIDs max 32, WPA passwords 8..63 (or empty
 * for an explicitly requested open AP). Callers serialize link operations. */
