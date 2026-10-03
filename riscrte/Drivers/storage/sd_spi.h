/* SD SPI protocol owned by this ELF: commands, addressing, tokens and CRC. */
#include "../common/spi.h"
static risc_hw_sd_spi_v1 config;
static bool have_config;
static uint32_t sd_sectors;
static bool sd_high_capacity;
static uint8_t crc7(const uint8_t *p,size_t n) {
    uint8_t crc=0;
    for (size_t i=0;i<n;i++) for (uint8_t mask=0x80;mask;mask>>=1) {
        crc<<=1; if (((p[i]&mask)!=0) != ((crc&0x80)!=0)) crc^=9;
    }
    return (uint8_t)((crc<<1)|1);
}
static uint16_t crc16(const uint8_t *p,size_t n) {
    uint16_t crc=0;
    for (size_t i=0;i<n;i++) { crc^=(uint16_t)p[i]<<8; for (uint8_t j=0;j<8;j++) crc=(crc&0x8000)?(uint16_t)((crc<<1)^0x1021):(uint16_t)(crc<<1); }
    return crc;
}
static bool sd_byte(uint8_t *out) { return spi->exchange(spi->context,spi_claim,NULL,out,1); }
static bool sd_command(uint8_t command,uint32_t arg,uint8_t *response) {
    uint8_t bytes[6]={0x40u|command,(uint8_t)(arg>>24),(uint8_t)(arg>>16),(uint8_t)(arg>>8),(uint8_t)arg,0};
    bytes[5]=crc7(bytes,5);
    uint8_t dummy;
    if (!sd_byte(&dummy) || !spi->exchange(spi->context,spi_claim,bytes,NULL,6)) return false;
    for (uint8_t i=0;i<16;i++) if (!sd_byte(response)) return false; else if (!(*response&0x80)) return true;
    return false;
}
static bool sd_token(uint8_t token) {
    uint64_t began=timer->monotonic_ms(timer->context);
    for (uint16_t i=0;i<512;i++) {
        uint8_t b; if (!sd_byte(&b)) return false;
        if (b==token) return true;
        if (token==0xfe && b!=0xff) return false;
        if ((i&15)==15) { if (timer->monotonic_ms(timer->context)-began>=15) return false; timer->sleep_ms(timer->context,1); }
    }
    return false;
}
static bool sd_receive(uint8_t *out,size_t count) {
    uint8_t crc[2];
    return sd_token(0xfe) && spi->exchange(spi->context,spi_claim,NULL,out,count) &&
        spi->exchange(spi->context,spi_claim,NULL,crc,2) && crc16(out,count)==((uint16_t)crc[0]<<8|crc[1]);
}
static bool sd_simple(uint8_t cmd,uint32_t arg,uint8_t expected,uint8_t *extra,size_t n) {
    uint8_t response=0xff;
    if (!spi_begin(400000)) return false;
    bool ok=sd_command(cmd,arg,&response) && response==expected && (!n || spi->exchange(spi->context,spi_claim,NULL,extra,n));
    return spi_end() && ok;
}
static bool sd_initialize(void) {
    sd_sectors=0;
    if(config.detect>=0 && gpio_read(config.detect)!=config.detect_active_high) return false;
    if (!spi->idle_clocks(spi->context,spi_claim,400000,80) || !sd_simple(0,0,1,NULL,0)) return false;
    uint8_t r7[4]; bool v2=sd_simple(8,0x1aa,1,r7,4);
    if (v2 && (r7[2]!=1 || r7[3]!=0xaa)) return false;
    uint64_t began=timer->monotonic_ms(timer->context); bool ready=false;
    for (uint16_t attempts=0;attempts<100;attempts++) {
        if (!sd_simple(55,0,1,NULL,0)) return false;
        uint8_t response=0xff;
        if (!spi_begin(400000)) return false;
        bool ok=sd_command(41,v2?0x40000000:0,&response); bool ended=spi_end();
        if (!ok || !ended || response>1) return false;
        if (!response) { ready=true; break; }
        if (timer->monotonic_ms(timer->context)-began>=1000) return false;
        timer->sleep_ms(timer->context,10);
    }
    if (!ready || !sd_simple(58,0,0,r7,4) || !(r7[0]&0x80)) return false;
    sd_high_capacity=v2 && (r7[0]&0x40);
    if (!sd_high_capacity && !sd_simple(16,512,0,NULL,0)) return false;
    uint8_t csd[16],response;
    if (!spi_begin(400000)) return false;
    bool ok=sd_command(9,0,&response) && !response && sd_receive(csd,16); bool ended=spi_end();
    if (!ok || !ended) return false;
    uint64_t sectors=0;
    if ((csd[0]>>6)==1) sectors=(((uint32_t)(csd[7]&63)<<16|((uint32_t)csd[8]<<8)|csd[9])+1ull)*1024;
    else if ((csd[0]>>6)==0) {
        uint32_t size=((uint32_t)(csd[6]&3)<<10)|((uint32_t)csd[7]<<2)|(csd[8]>>6);
        uint8_t mult=((csd[9]&3)<<1)|(csd[10]>>7),read_len=csd[5]&15;
        sectors=((uint64_t)(size+1)<<(mult+2+read_len))/512;
    }
    if (!sectors || sectors>=UINT32_MAX) return false;
    sd_sectors=(uint32_t)sectors; return true;
}
static bool sd_read(uint32_t lba,uint8_t *out) {
    if (lba>=sd_sectors || (!sd_high_capacity && lba>UINT32_MAX/512) || !spi_begin(10000000)) return false;
    uint8_t r=0xff;
    bool ok=sd_command(17,sd_high_capacity?lba:lba*512,&r) && !r && sd_receive(out,512);
    return spi_end() && ok;
}
static bool sd_write(uint32_t lba,const uint8_t *bytes) {
    if(config.write_protect>=0 && gpio_read(config.write_protect)==config.write_protect_active_high) return false;
    if (lba>=sd_sectors || (!sd_high_capacity && lba>UINT32_MAX/512) || !spi_begin(10000000)) return false;
    uint8_t r=0xff,token=0xfe; uint16_t sum=crc16(bytes,512); uint8_t crc[2]={(uint8_t)(sum>>8),(uint8_t)sum};
    bool ok=sd_command(24,sd_high_capacity?lba:lba*512,&r) && !r &&
        spi->exchange(spi->context,spi_claim,&token,NULL,1) && spi->exchange(spi->context,spi_claim,bytes,NULL,512) &&
        spi->exchange(spi->context,spi_claim,crc,NULL,2) && sd_byte(&r) && (r&31)==5 && sd_token(0xff);
    return spi_end() && ok;
}
