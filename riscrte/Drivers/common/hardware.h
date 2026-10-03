#pragma once
#include "dependency.h"
#include "RiscHardwareConfigV1.h"
static inline const void *hardware_config(const risc_provider_dependency_v1 *d,size_t n,
        const char *compatible,const char *type,size_t size) {
    const risc_hardware_device_v1 *entry=garden_dependency(d,n,"hardware.device",sizeof(*entry));
    if (!entry || !entry->instance_id || !entry->compatible || strcmp(entry->compatible,compatible) ||
        !entry->revision || strcmp(entry->revision,"unspecified") || !entry->config_type || strcmp(entry->config_type,type) ||
        entry->config_version!=1 || entry->config_size<size || !entry->config || *(const uint32_t *)entry->config<size || *(const uint32_t *)entry->config>entry->config_size) return NULL;
    return entry->config;
}
static inline bool hw_pin(int16_t p) { return p>=0 && p<49; }
static inline bool hw_unique(const int16_t *p,size_t n) {
    for(size_t i=0;i<n;i++) { if(p[i]==-1) continue; if(!hw_pin(p[i])) return false; for(size_t j=0;j<i;j++) if(p[i]==p[j]) return false; }
    return true;
}
static inline bool hw_bus(const risc_hw_bus_v1 *b,uint32_t kind) {
    if(b->struct_size<sizeof(*b) || b->kind!=kind || !b->instance_id || b->controller>3 || !b->frequency_hz ||
       b->reserved[0] || b->reserved[1] || b->reserved[2] || b->mode) return false;
    if(kind==RISC_HW_BUS_SPI) {
        int16_t p[]={b->sclk,b->mosi,b->miso};
        return hw_pin(b->sclk) && hw_pin(b->mosi) && b->sda==-1 && b->scl==-1 && hw_unique(p,3) && b->frequency_hz<=10000000;
    }
    int16_t p[]={b->sda,b->scl};
    return hw_pin(b->sda) && hw_pin(b->scl) && b->sclk==-1 && b->mosi==-1 && b->miso==-1 && hw_unique(p,2) && b->frequency_hz<=400000;
}
static inline bool hw_bank(const risc_hw_gpio_bank_v1 *b,uint8_t maximum) {
    if(b) for(size_t i=0;i<b->count && i<8;i++) if(!hw_pin(b->pins[i])) return false;
    return b && b->count && b->count<=maximum && b->active_high<=1 && b->pull_up<=1 && !b->reserved && hw_unique(b->pins,b->count) &&
        b->debounce_us<=10000000 && b->long_press_us<=60000000 && b->click_min_us<=60000000;
}
static inline bool hw_display(const risc_hw_spi_display_v1 *c,uint16_t w,uint16_t h,bool paper) {
    if(!c || c->width!=w || c->height!=h || c->offset_x || c->offset_y || c->rotation || c->reserved[0] || c->reserved[1] || c->reserved[2] || !hw_bus(&c->bus,RISC_HW_BUS_SPI) || !hw_pin(c->cs) || !hw_pin(c->dc) || !hw_pin(c->reset) ||
       c->reset_active_high>1 || c->busy_active_high>1 || c->backlight_active_high>1 || c->power_count>4 ||
       !c->reset_assert_ms || c->reset_assert_ms>500 || !c->reset_recovery_ms || c->reset_recovery_ms>500 ||
       (paper && !hw_pin(c->busy)) || (!paper && c->busy!=-1) || (paper && c->backlight!=-1)) return false;
    int16_t p[12]={c->bus.sclk,c->bus.mosi,c->bus.miso,c->cs,c->dc,c->reset,c->backlight,c->busy,-1,-1,-1,-1};
    for(size_t i=0;i<c->power_count;i++) { if(!hw_pin(c->power_pins[i]) || c->power_active_high[i]>1) return false;p[8+i]=c->power_pins[i]; }
    return hw_unique(p,12);
}
