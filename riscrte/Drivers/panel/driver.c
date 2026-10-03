#include "../common/spi.h"
#include "init_registers.h"
#define WIDTH 240u
#define HEIGHT 240u
#define STRIDE 480u
#define FORMAT RISC_DISPLAY_FORMAT_RGB565
#define DISPLAY_FLAGS (RISC_DISPLAY_INFO_ASYNC_PRESENT|RISC_DISPLAY_INFO_BRIGHTNESS)
#define DISPLAY_ID "panel"
static bool hw_start(const risc_provider_dependency_v1 *d,size_t n) {
    if (!spi_dependencies(d,n) || garden_board(d,n)!=GARDEN_BOARD_DIAL) return false;
    if (!gpio_output(1) || !gpio_output(2) || !gpio_output(3) || !gpio_output(14) || !gpio_output(46)) return false;
    gpio_write(1,true); gpio_write(2,true);
    if (!spi->claim(spi->context,10,11,-1,9,&spi_claim) || !spi_claim) return false;
    gpio_write(14,false); timer->sleep_ms(timer->context,200);
    gpio_write(14,true); timer->sleep_ms(timer->context,200);
    for (size_t i=0;i<sizeof(init_registers)/sizeof(init_registers[0]);i++) {
        if (!display_command(3,init_registers[i].command,init_registers[i].bytes,init_registers[i].count)) return false;
        timer->sleep_ms(timer->context,1);
    }
    if (!display_command(3,0x11,NULL,0)) return false;
    timer->sleep_ms(timer->context,120);
    if (!display_command(3,0x29,NULL,0)) return false;
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
        gpio_write(3,false); ok=!io_fault && spi->exchange(spi->context,spi_claim,&commands[i],NULL,1);
        if (ok) { gpio_write(3,true); ok=!io_fault && spi->exchange(spi->context,spi_claim,data[i],NULL,sizes[i]); }
    }
    return spi_end() && ok;
}
static int hw_finish(void) { return 1; }
static bool hw_brightness(uint16_t level,uint16_t maximum) { return gpio->pwm(gpio->context,pins[46],1000,level,maximum); }
static bool hw_stop(void) {
    io_fault=false;
    if (pins[46]) gpio_write(46,false);
    if (pins[14]) gpio_write(14,false);
    if (pins[1]) gpio_write(1,false);
    if (pins[2]) gpio_write(2,false);
    if (io_fault || !spi_release()) return false;
    for (uint8_t p=0;p<49;p++) gpio_release(p);
    return gpio_clean();
}
#include "../common/display.h"
