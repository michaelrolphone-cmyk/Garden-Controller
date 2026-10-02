#pragma once
/* net.wifi@1. Generic station link. Radio comes from a provider. */
#include <stdbool.h>
#include <stdint.h>
#define WIFI_API_V1 1u
typedef enum { WIFI_LINK_DOWN = 0, WIFI_LINK_JOINING, WIFI_LINK_UP } wifi_link_t;
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    bool (*connect)(void *context, const char *ssid, const char *password);
    void (*disconnect)(void *context);
    wifi_link_t (*status)(void *context);
    int8_t (*rssi)(void *context);
} wifi_api_v1;
