#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "GardenPlatformV1.h"
#include "RiscPlatformClockV1.h"
#include "RiscI2cBusV1.h"
#include "RiscDisplayOutputV1.h"
#include "RiscStorageVolumeV1.h"
#include "RiscTouchV1.h"
#include "button/ButtonApi.h"
static uint64_t mock_now,mock_serial=100;
static uint64_t mock_pins[49];
static bool mock_levels[49],mock_release_fail,mock_write_fail;
static int mock_fail_pin=-1;
static size_t mock_wave_count,mock_spi_bytes,mock_rows;
static uint8_t mock_command,mock_radio_state,mock_touch[5];
static uint8_t mock_first[2],mock_last[2];
static uint64_t mock_time(void *c) { (void)c; return mock_now; }
static void mock_sleep(void *c,uint32_t ms) { (void)c; mock_now+=ms; }
static bool mock_claim(void *c,uint8_t p,bool output,bool initial,bool pull,uint64_t *t) {
    (void)c;(void)pull; if (p>=49 || mock_pins[p] || p==mock_fail_pin) return false;
    *t=mock_pins[p]=++mock_serial; mock_levels[p]=output?initial:true; return true;
}
static int mock_pin(uint64_t t) { for (int i=0;i<49;i++) if (mock_pins[i]==t && t) return i; return -1; }
static bool mock_write(void *c,uint64_t t,bool v) { (void)c; int p=mock_pin(t); if (p<0 || mock_write_fail) return false; mock_levels[p]=v; return true; }
static bool mock_read(void *c,uint64_t t,bool *v) { (void)c; int p=mock_pin(t); if (p<0) return false; *v=mock_levels[p]; return true; }
static bool mock_pwm(void *c,uint64_t t,uint32_t hz,uint16_t duty,uint16_t max) { (void)c;assert(hz && max && duty<=max);return mock_write(c,t,duty!=0); }
static bool mock_release(void *c,uint64_t t) { (void)c; int p=mock_pin(t); if (p<0 || mock_release_fail) return false; mock_pins[p]=0;return true; }
static bool mock_wave(void *c,uint64_t t,const uint32_t *ns,size_t n) { (void)c;assert(mock_pin(t)>=0 && ns && n && n<=768);mock_wave_count=n;return true; }
static bool mock_spi_claim(void *c,uint8_t clk,uint8_t mosi,int8_t miso,uint8_t cs,uint64_t *t) { (void)c;(void)clk;(void)mosi;(void)miso;*t=cs+1;return true; }
static bool mock_spi_begin(void *c,uint64_t t,uint32_t hz,uint8_t mode,uint32_t timeout) { (void)c;assert(t && hz && !mode && timeout);return true; }
static bool mock_spi_end(void *c,uint64_t t) { (void)c;assert(t);return true; }
static bool mock_spi_release(void *c,uint64_t t) { (void)c;assert(t);return !mock_release_fail; }
static bool mock_idle(void *c,uint64_t t,uint32_t hz,uint16_t clocks) { (void)c;assert(t && hz && clocks>=74);return true; }
static bool mock_exchange(void *c,uint64_t t,const uint8_t *tx,uint8_t *rx,size_t n);
static bool mock_i2c_claim(void *c,uint8_t address,uint64_t *t) { (void)c;assert(address==0x15);*t=++mock_serial;return true; }
static bool mock_i2c_transfer(void *c,uint64_t t,const uint8_t *tx,size_t nt,uint8_t *rx,size_t nr,uint32_t timeout) {
    (void)c;assert(t && tx && nt==1 && timeout==20);
    if (*tx==0xa7) { assert(nr==1);*rx=0xb6;return true; }
    assert(*tx==2 && nr==5);memcpy(rx,mock_touch,5);return true;
}
static bool mock_i2c_release(void *c,uint64_t t) { (void)c;assert(t);return !mock_release_fail; }
static bool mock_radio_claim(void *c,uint64_t *t) { (void)c;*t=++mock_serial;return true; }
static bool mock_join(void *c,uint64_t t,const char *s,const char *p) { (void)c;assert(t && s && p);mock_radio_state=1;return true; }
static bool mock_radio_status(void *c,uint64_t t,uint8_t *state,int8_t *rssi) { (void)c;assert(t);*state=mock_radio_state;*rssi=-42;return true; }
static bool mock_radio_leave(void *c,uint64_t t) { (void)c;assert(t);mock_radio_state=0;return !mock_release_fail; }
static bool mock_radio_release(void *c,uint64_t t) { (void)c;assert(t);return !mock_release_fail; }
static bool mock_ap_start(void *c,uint64_t t,const char *ssid,const char *password,const uint8_t a[4],const uint8_t g[4],const uint8_t m[4]) { (void)c; assert(t && ssid && password && a && g && m); return true; }
static bool mock_addresses(void *c,uint64_t t,uint8_t station[12],uint8_t ap[12]) { (void)c;assert(t);memset(station,0,12);memset(ap,0,12);ap[0]=192;ap[1]=168;ap[2]=4;ap[3]=1;return true; }
static uint8_t mock_button_count(void *c) { (void)c;return 1; }
static bool mock_button(void *c,uint8_t ch) { (void)c;assert(ch==0);return true; }
static void mock_button_poll(void *c,uint32_t now) { (void)c;(void)now; }
static garden_board_v1 mock_board={1,sizeof(mock_board),1};
static garden_gpio_v1 mock_gpio={1,sizeof(mock_gpio),NULL,mock_claim,mock_write,mock_read,mock_pwm,mock_release,mock_wave};
static risc_platform_clock_api_v1 mock_clock={1,sizeof(mock_clock),NULL,mock_time,mock_sleep};
static garden_spi_v1 mock_spi={1,sizeof(mock_spi),NULL,mock_spi_claim,mock_spi_begin,mock_exchange,mock_spi_end,mock_idle,mock_spi_release};
static risc_i2c_bus_api_v1 mock_i2c={1,sizeof(mock_i2c),NULL,mock_i2c_claim,mock_i2c_transfer,mock_i2c_release};
static garden_radio_v1 mock_radio={1,sizeof(mock_radio),NULL,mock_radio_claim,mock_join,mock_radio_status,mock_radio_leave,mock_radio_release,mock_ap_start,mock_radio_leave,mock_addresses};
static button_api_v1 mock_buttons={1,sizeof(mock_buttons),NULL,mock_button_count,mock_button_poll,mock_button,mock_button,mock_button};
static risc_provider_dependency_v1 mock_deps[]={
 {"board.garden",1,&mock_board},{"platform.board",1,&mock_board},{"platform.gpio",1,&mock_gpio},{"platform.clock",1,&mock_clock},
 {"spi.bus",1,&mock_spi},{"i2c.bus",1,&mock_i2c},{"platform.radio",1,&mock_radio},{"input.button",1,&mock_buttons}
};
#define MOCK_N (sizeof(mock_deps)/sizeof(mock_deps[0]))
