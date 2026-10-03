#include "PixelApi.h"
#include "../common/gpio.h"
static bool running;
static uint8_t pin,count_value,brightness,order;
static uint8_t frame[PIXEL_CHANNELS_MAX*3];
static uint8_t count(void *c) { (void)c; return running ? count_value : 0; }
static bool set_rgb(void *c,uint8_t i,uint8_t r,uint8_t g,uint8_t b) {
    (void)c; if (!running || i>=count_value) return false;
    frame[i*3]=order?r:g; frame[i*3+1]=order?g:r; frame[i*3+2]=b; return true;
}
static bool set_brightness(void *c,uint8_t percent) { (void)c; if (!running || percent>100) return false; brightness=percent; return true; }
static bool transmit(void) {
    uint32_t ns[PIXEL_CHANNELS_MAX*48];
    for (size_t i=0;i<(size_t)count_value*3;i++) {
        uint8_t v=(uint8_t)((frame[i]*brightness)/100);
        for (size_t bit=0;bit<8;bit++) {
            bool one=(v&(0x80u>>bit))!=0;
            ns[i*16+bit*2]=one?800:400;
            ns[i*16+bit*2+1]=one?450:850;
        }
    }
    bool ok=gpio->waveform(gpio->context,pins[pin],ns,(size_t)count_value*48);
    gpio_write(pin,false); timer->sleep_ms(timer->context,1);
    return ok && !io_fault;
}
static bool show(void *c) { (void)c; return running && transmit(); }
static const pixel_api_v1 api={1,sizeof(api),NULL,count,set_rgb,set_brightness,show};
static bool start(const risc_provider_dependency_v1 *d,size_t n) {
    if (running || !gpio_dependencies(d,n)) return false;
    const risc_hw_pixel_v1 *config=hardware_config(d,n,"protocol,ws2812-800khz","pixel.ws2812",sizeof(*config));
    if (!config || !hw_pin(config->pin) || !config->count || config->count>PIXEL_CHANNELS_MAX || config->order>1) return false;
    pin=(uint8_t)config->pin; count_value=config->count; order=config->order; gpio_output_initial=false;
    if (!gpio_output(pin)) return false;
    memset(frame,0,sizeof(frame)); brightness=100;
    running=transmit(); return running;
}
static bool quiesce(void) {
    running=false; io_fault=false;
    if (!pins[pin]) return true;
    memset(frame,0,sizeof(frame)); if (!transmit()) return false;
    gpio_release(pin); return gpio_clean();
}
static void stop(void) { (void)quiesce(); }
static const risc_driver_v2 driver={2,sizeof(driver),"pixel","indicator.pixel",1,&api,start,stop,quiesce};
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi==2?&driver:NULL; }
