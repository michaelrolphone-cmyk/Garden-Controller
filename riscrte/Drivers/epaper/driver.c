#include "../common/spi.h"
#define WIDTH 800u
#define HEIGHT 480u
#define STRIDE 100u
#define FORMAT RISC_DISPLAY_FORMAT_MONO1
#define DISPLAY_FLAGS (RISC_DISPLAY_INFO_ASYNC_PRESENT|RISC_DISPLAY_INFO_RETAINS_IMAGE|RISC_DISPLAY_INFO_CLEAN_PRESENT)
#define DISPLAY_ID "epaper"
static uint8_t refresh_phase;
static uint64_t phase_started;
static bool command(uint8_t cmd,const uint8_t *data,size_t n) { return display_command(3,cmd,data,n); }
static bool hw_start(const risc_provider_dependency_v1 *d,size_t n) {
    if (!spi_dependencies(d,n) || garden_board(d,n)!=GARDEN_BOARD_PAPER) return false;
    if (!gpio_output(3) || !gpio_output(6) || !gpio_input(2)) return false;
    if (!spi->claim(spi->context,5,4,19,7,&spi_claim) || !spi_claim) return false;
    gpio_write(6,false); timer->sleep_ms(timer->context,10); gpio_write(6,true); timer->sleep_ms(timer->context,10);
    const uint8_t panel[]={0x1f},power[]={7,7,0x3f,0x3f,9},booster[]={0x17,0x17,0x28,0x17},resolution[]={3,0x20,1,0xe0},zero[]={0},interval[]={0x29,7},timing[]={0x22};
    bool ok=command(0,panel,1) && command(1,power,5) && command(6,booster,4) && command(0x61,resolution,4) && command(0x15,zero,1) && command(0x50,interval,2) && command(0x60,timing,1) && command(0xe3,timing,1) && command(0xe0,zero,1) && command(0x41,zero,1);
    if (!ok || !command(0x10,NULL,0)) return false;
    uint8_t white[100]; memset(white,0xff,sizeof(white));
    uint64_t began=timer->monotonic_ms(timer->context);
    for (uint32_t row=0;row<480;row++) {
        if (timer->monotonic_ms(timer->context)-began>2000 || !spi_begin(10000000)) return false;
        gpio_write(3,true);
        bool sent=!io_fault && spi->exchange(spi->context,spi_claim,white,NULL,sizeof(white));
        if (!spi_end() || !sent) return false;
        if ((row&7)==7) timer->sleep_ms(timer->context,1);
    }
    return true;
}
static void hw_submit(void) { refresh_phase=0; }
static bool hw_row(uint32_t y,const uint8_t *pixels) {
    uint8_t wire[100]; for (size_t i=0;i<100;i++) wire[i]=(uint8_t)~pixels[i];
    if (!y && !command(0x13,NULL,0)) return false;
    if (!spi_begin(10000000)) return false;
    gpio_write(3,true);
    bool ok=!io_fault && spi->exchange(spi->context,spi_claim,wire,NULL,100);
    return spi_end() && ok;
}
static int hw_finish(void) {
    uint64_t now=timer->monotonic_ms(timer->context);
    if (!refresh_phase) {
        if (!command(4,NULL,0)) return -1;
        refresh_phase=1; phase_started=now; return 0;
    }
    /* BUSY is active LOW. Minimum settle prevents observing the pre-command HIGH. */
    if (now-phase_started<10) return 0;
    if (!gpio_read(2)) return now-phase_started>15000?-1:0;
    if (io_fault) return -1;
    if (refresh_phase==1) {
        if (!command(0x12,NULL,0)) return -1;
        refresh_phase=2; phase_started=now; return 0;
    }
    if (refresh_phase==2) {
        if (!command(2,NULL,0)) return -1;
        refresh_phase=3; phase_started=now; return 0;
    }
    refresh_phase=0; return 1;
}
static bool hw_brightness(uint16_t level,uint16_t maximum) { (void)level;(void)maximum; return false; }
static bool hw_stop(void) {
    /* Reset terminates a failed refresh before raw resources can be released. */
    io_fault=false;
    if (pins[6]) { gpio_write(6,false); timer->sleep_ms(timer->context,10); }
    if (io_fault || !spi_release()) return false;
    for (uint8_t p=0;p<49;p++) gpio_release(p);
    return gpio_clean();
}
#include "../common/display.h"
