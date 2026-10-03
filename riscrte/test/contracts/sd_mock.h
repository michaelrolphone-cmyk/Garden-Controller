/* Minimal reference SDHC + FAT16 image. Commands are parsed from transmitted
 * bytes, not from driver internals. Reads include independently computed CRC. */
static uint8_t disk[8192][512],reply[520];
static size_t reply_n,reply_i;
static uint32_t write_lba;
static int write_phase;
static uint16_t fixture_crc(const uint8_t *p,size_t n) {
    uint16_t v=0;for(size_t i=0;i<n;i++){v^=(uint16_t)p[i]<<8;for(int j=0;j<8;j++)v=(v&0x8000)?(uint16_t)(v*2^0x1021):(uint16_t)(v*2);}return v;
}
static void fixture_data(const uint8_t *p,size_t n) {
    reply[reply_n++]=0xfe;memcpy(reply+reply_n,p,n);reply_n+=n;
    uint16_t crc=fixture_crc(p,n);reply[reply_n++]=(uint8_t)(crc>>8);reply[reply_n++]=(uint8_t)crc;
}
static void fixture_disk(void) {
    uint8_t *b=disk[0];b[0]=0xeb;b[11]=0;b[12]=2;b[13]=1;b[14]=1;b[16]=1;
    b[17]=0;b[18]=2;b[19]=0;b[20]=32;b[21]=0xf8;b[22]=32;b[510]=0x55;b[511]=0xaa;
    disk[1][0]=0xf8;disk[1][1]=0xff;disk[1][2]=0xff;disk[1][3]=0xff;
    disk[1][4]=disk[1][5]=0xff;
    memcpy(disk[33],"HELLO   TXT",11);disk[33][26]=2;disk[33][28]=5;
    memcpy(disk[65],"hello",5);
}
static bool mock_exchange(void *c,uint64_t t,const uint8_t *tx,uint8_t *rx,size_t n) {
    (void)c;assert(t && n<=512);
    if (tx && n==6 && (tx[0]&0xc0)==0x40) {
        uint8_t cmd=tx[0]&63;uint32_t arg=(uint32_t)tx[1]<<24|(uint32_t)tx[2]<<16|(uint32_t)tx[3]<<8|tx[4];
        reply_i=reply_n=0;reply[reply_n++]=(cmd==0 || cmd==8 || cmd==55)?1:0;
        if(cmd==0)assert(tx[5]==0x95);
        if(cmd==8){assert(tx[5]==0x87);uint8_t r[]={0,0,1,0xaa};memcpy(reply+reply_n,r,4);reply_n+=4;}
        if(cmd==58){uint8_t r[]={0xc0,0xff,0x80,0};memcpy(reply+reply_n,r,4);reply_n+=4;}
        if(cmd==9){uint8_t csd[16]={0x40};csd[9]=7;fixture_data(csd,16);}
        if(cmd==17){assert(arg<8192);fixture_data(disk[arg],512);}
        if(cmd==24){assert(arg<8192);write_lba=arg;write_phase=1;}
        return true;
    }
    if(tx && write_phase){
        if(write_phase==1){assert(n==1 && *tx==0xfe);write_phase=2;}
        else if(write_phase==2){assert(n==512);memcpy(disk[write_lba],tx,512);write_phase=3;}
        else {assert(n==2);uint16_t crc=fixture_crc(disk[write_lba],512);assert(tx[0]==crc>>8 && tx[1]==(uint8_t)crc);write_phase=0;reply_i=0;reply_n=1;reply[0]=5;}
        return true;
    }
    if(rx)for(size_t i=0;i<n;i++)rx[i]=reply_i<reply_n?reply[reply_i++]:0xff;
    return true;
}
