#include "../common/spi.h"
static risc_hw_spi_display_v1 config;
static bool have_config;
#include "init_registers.h"
#define WIDTH 240u
#define HEIGHT 240u
#define STRIDE 480u
#define FORMAT RISC_DISPLAY_FORMAT_RGB565
#define DISPLAY_FLAGS (RISC_DISPLAY_INFO_ASYNC_PRESENT|(config.backlight>=0?RISC_DISPLAY_INFO_BRIGHTNESS:0))
#define DISPLAY_ID "panel"
static bool hw_start(const risc_provider_dependency_v1 *d,size_t n) {
    const risc_hw_spi_display_v1 *next=hardware_config(d,n,"galaxycore,gc9a01","display.spi",sizeof(*next));
    if (!hw_display(next,240,240,false)) return false;
    if (!spi_dependencies(d,n)) return false;
    config=*next; have_config=true; spi_max_hz=config.bus.frequency_hz;
    gpio_output_initial=false;
    if (!gpio_output(config.dc)) return false;
    gpio_output_initial=config.reset_active_high;
    if (!gpio_output(config.reset)) return false;
    for(size_t i=0;i<config.power_count;i++) {
        gpio_output_initial=!config.power_active_high[i];
        if(!gpio_output(config.power_pins[i])) return false;
    }
    if(config.backlight>=0) { gpio_output_initial=!config.backlight_active_high; if(!gpio_output(config.backlight)) return false; }
    if(config.busy>=0 && !gpio_input(config.busy)) return false;
    if (!spi->claim(spi->context,config.bus.sclk,config.bus.mosi,config.bus.miso,config.cs,&spi_claim) || !spi_claim) return false;
    for(size_t i=0;i<config.power_count;i++) gpio_write(config.power_pins[i],config.power_active_high[i]);
    gpio_write(config.reset,config.reset_active_high); timer->sleep_ms(timer->context,config.reset_assert_ms);
    gpio_write(config.reset,!config.reset_active_high); timer->sleep_ms(timer->context,config.reset_recovery_ms);
    for (size_t i=0;i<sizeof(init_registers)/sizeof(init_registers[0]);i++) {
        if (!display_command(config.dc,init_registers[i].command,init_registers[i].bytes,init_registers[i].count)) return false;
        timer->sleep_ms(timer->context,1);
    }
    if (!display_command(config.dc,0x11,NULL,0)) return false;
    timer->sleep_ms(timer->context,120);
    if (!display_command(config.dc,0x29,NULL,0)) return false;
    timer->sleep_ms(timer->context,20);
    return !io_fault;
}
static void hw_submit(void) {}
static bool hw_row(uint32_t y,const uint8_t *pixels) {
    uint8_t x[4]={0,0,0,239},row[4]={0,(uint8_t)y,0,(uint8_t)y},wire[480];
    /* Canonical RGB565 is native-endian; GC9A01 wire order is MSB first. */
    for (size_t i=0;i<480;i+=2) { uint16_t v; memcpy(&v,pixels+i,2); wire[i]=(uint8_t)(v>>8); wire[i+1]=(uint8_t)v; }
    if (!spi_begin(10000000)) return false;
    uint8_t commands[]={0x2a,0x2b,0x2c};
    const uint8_t *data[]={x,row,wire}; const size_t sizes[]={4,4,sizeof(wire)};
    bool ok=true;
    for (size_t i=0;i<3 && ok;i++) {
        gpio_write(config.dc,false); ok=!io_fault && spi->exchange(spi->context,spi_claim,&commands[i],NULL,1);
        if (ok) { gpio_write(config.dc,true); ok=!io_fault && spi->exchange(spi->context,spi_claim,data[i],NULL,sizes[i]); }
    }
    return spi_end() && ok;
}
static int hw_finish(void) { return 1; }
static bool hw_brightness(uint16_t level,uint16_t maximum) { return config.backlight>=0 && gpio->pwm(gpio->context,pins[config.backlight],1000,config.backlight_active_high?level:maximum-level,maximum); }
static bool hw_stop(void) {
    io_fault=false;
    if(have_config) {
        if(config.backlight>=0 && pins[config.backlight]) gpio_write(config.backlight,!config.backlight_active_high);
        if(pins[config.reset]) { gpio_write(config.reset,config.reset_active_high); timer->sleep_ms(timer->context,config.reset_assert_ms); }
        for(size_t i=0;i<config.power_count;i++) if(pins[config.power_pins[i]]) gpio_write(config.power_pins[i],!config.power_active_high[i]);
    }
    if (io_fault || !spi_release()) return false;
    for (uint8_t p=0;p<49;p++) gpio_release(p);
    if (!gpio_clean()) return false;
    have_config=false;return true;
}
#include "../common/display.h"
