#pragma once
#include "gpio.h"
static const garden_spi_v1 *spi;
static uint64_t spi_claim;
static bool spi_held;
static uint32_t spi_timeout_ms=20;
static inline bool spi_dependencies(const risc_provider_dependency_v1 *d,size_t n) {
    if (spi_claim || !gpio_dependencies(d,n)) return false;
    spi=garden_dependency(d,n,"spi.bus",sizeof(*spi));
    return spi && spi->claim && spi->begin && spi->exchange && spi->end && spi->idle_clocks && spi->release;
}
static inline bool spi_begin(uint32_t hz) {
    if (spi_held) return false;
    spi_held=spi->begin(spi->context,spi_claim,hz,0,spi_timeout_ms); return spi_held;
}
static inline bool spi_end(void) {
    if (!spi_held) return true;
    if (!spi->end(spi->context,spi_claim)) return false;
    spi_held=false; return true;
}
static inline bool spi_release(void) {
    if (!spi_end()) return false;
    if (spi_claim && !spi->release(spi->context,spi_claim)) return false;
    spi_claim=0; return true;
}
static inline bool display_command(uint8_t dc,uint8_t cmd,const uint8_t *bytes,size_t count) {
    if (!spi_begin(10000000)) return false;
    gpio_write(dc,false);
    bool ok=!io_fault && spi->exchange(spi->context,spi_claim,&cmd,NULL,1);
    if (count && ok) { gpio_write(dc,true); ok=!io_fault && spi->exchange(spi->context,spi_claim,bytes,NULL,count); }
    bool ended=spi_end(); return ok && ended;
}
