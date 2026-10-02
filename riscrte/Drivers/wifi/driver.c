#include "WifiApi.h"
#include "../../sdk/RiscProviderV2.h"
#include <string.h>
typedef struct { bool (*join)(const char *ssid, const char *password); void (*leave)(void); int8_t (*rssi)(void); } wifi_port_t;
static bool running, have_port;
static wifi_port_t port;
static wifi_link_t link;
static char ssid[33];
bool wifi_bind_port(const wifi_port_t *next) { if (running || !next || !next->join || !next->leave) return false; port = *next; have_port = true; return true; }
static bool connect(void *c, const char *name, const char *password) { (void)c; if (!running || !name || !name[0] || strlen(name) > 32) return false; if (!port.join(name, password)) { link = WIFI_LINK_DOWN; return false; } strncpy(ssid, name, 32); link = WIFI_LINK_UP; return true; }
static void disconnect(void *c) { (void)c; if (running) { port.leave(); link = WIFI_LINK_DOWN; } }
static wifi_link_t status(void *c) { (void)c; return running ? link : WIFI_LINK_DOWN; }
static int8_t rssi(void *c) { (void)c; return running && link == WIFI_LINK_UP && port.rssi ? port.rssi() : -127; }
static const wifi_api_v1 api = { WIFI_API_V1, sizeof(wifi_api_v1), NULL, connect, disconnect, status, rssi };
static bool start(const risc_provider_dependency_v1 *d, size_t n) { (void)d; if (running || n || !have_port) return false; link = WIFI_LINK_DOWN; running = true; return true; }
static bool quiesce(void) { if (running) disconnect(NULL); return true; }
static void stop(void) { (void)quiesce(); running = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "wifi", "net.wifi", WIFI_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
