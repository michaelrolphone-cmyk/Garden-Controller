#include "WifiApi.h"
#include "../common/hardware.h"
static risc_hw_radio_v1 hardware;
static const garden_radio_v1 *radio;
static uint64_t claim;
static bool running;
static bool bounded(const char *s,size_t maximum) {
    if (!s) return false;
    for (size_t i=0;i<=maximum;i++) if (!s[i]) return true;
    return false;
}
static bool connect(void *c,const char *ssid,const char *password) {
    (void)c;
    return running && (hardware.features&1) && ssid && ssid[0] && bounded(ssid,32) && bounded(password,63) &&
        radio->join(radio->context,claim,ssid,password);
}
static void disconnect(void *c) { (void)c; if (running) (void)radio->leave(radio->context,claim); }
static wifi_link_t status(void *c) {
    (void)c; uint8_t state=0; int8_t rssi=-127;
    if (!running || !radio->state(radio->context,claim,&state,&rssi) || state>WIFI_LINK_UP) return WIFI_LINK_DOWN;
    return (wifi_link_t)state;
}
static int8_t rssi(void *c) {
    (void)c; uint8_t state=0; int8_t value=-127;
    return running && radio->state(radio->context,claim,&state,&value) && state==WIFI_LINK_UP ? value : -127;
}
static bool start_ap(void *c,const char *ssid,const char *password,const wifi_ipv4_v1 *config) {
    (void)c;
    if (!running || !(hardware.features&2) || !ssid || !ssid[0] || !bounded(ssid,32) || !bounded(password,63) || !config) return false;
    size_t length=strlen(password); if (length && length<8) return false;
    return radio->start_ap(radio->context,claim,ssid,password,config->address,config->gateway,config->netmask);
}
static bool stop_ap(void *c) { (void)c; return running && radio->stop_ap(radio->context,claim); }
static bool addresses(void *c,wifi_ipv4_v1 *station,wifi_ipv4_v1 *ap) {
    (void)c; return running && station && ap && radio->addresses(radio->context,claim,(uint8_t *)station,(uint8_t *)ap);
}
static const wifi_api_v1 api={1,sizeof(api),NULL,connect,disconnect,status,rssi,start_ap,stop_ap,addresses};
static bool start(const risc_provider_dependency_v1 *d,size_t n) {
    if (claim || running) return false;
    const risc_hw_radio_v1 *next=hardware_config(d,n,"espressif,esp32s3-wifi","radio.integrated",sizeof(*next));
    if(!next || next->unit>3 || !next->features || (next->features&~3u)) return false;
    hardware=*next;
    radio=garden_dependency(d,n,"platform.radio",sizeof(*radio));
    if (!radio || !radio->claim || !radio->join || !radio->state || !radio->leave || !radio->release || !radio->start_ap || !radio->stop_ap || !radio->addresses) return false;
    running=radio->claim(radio->context,&claim) && claim;
    return running;
}
static bool quiesce(void) {
    running=false;
    if (!claim) return true;
    if (!radio->stop_ap(radio->context,claim) || !radio->leave(radio->context,claim) || !radio->release(radio->context,claim)) return false;
    claim=0; return true;
}
static void stop(void) { if (quiesce()) radio=NULL; }
static const risc_driver_v2 driver={2,sizeof(driver),"wifi","net.wifi",1,&api,start,stop,quiesce};
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi==2 ? &driver : NULL; }
