#include "mock.h"
#include DRIVER_SOURCE
#if TEST_ID==9
#include "sd_mock.h"
#else
static bool mock_exchange(void *c,uint64_t t,const uint8_t *tx,uint8_t *rx,size_t n) {
    (void)c;assert(t && n<=512);if(rx)memset(rx,0xff,n);
    if (tx && !mock_levels[3] && n==1) mock_command=*tx;
    else if (tx && mock_levels[3] && ((TEST_ID==7 && mock_command==0x2c)||(TEST_ID==8 && mock_command==0x13))) {
        if(!mock_spi_bytes){mock_first[0]=tx[0];mock_first[1]=tx[1];}
        mock_last[0]=tx[n-2];mock_last[1]=tx[n-1];mock_spi_bytes+=n;mock_rows++;
    }
    return true;
}
#endif
int main(void) {
    const risc_driver_v2 *d=t5_driver_get(2);assert(d && !t5_driver_get(1));
    assert(!d->start(NULL,0));assert(d->quiesce());d->stop();
    mock_board.board=TEST_BOARD;
#if TEST_ID==9
    fixture_disk();
#endif
    /* A present capability with a truncated table is not admissible. */
    uint32_t old_gpio_size=mock_gpio.struct_size,old_board_size=mock_board.struct_size,old_radio_size=mock_radio.struct_size;
    mock_gpio.struct_size=mock_board.struct_size=mock_radio.struct_size=0;
    assert(!d->start(mock_deps,MOCK_N));assert(d->quiesce());d->stop();
    mock_gpio.struct_size=old_gpio_size;mock_board.struct_size=old_board_size;mock_radio.struct_size=old_radio_size;
    assert(d->start(mock_deps,MOCK_N));assert(!d->start(mock_deps,MOCK_N));
#if TEST_ID==1
    const relay_api_v1 *a=d->capability;assert(a->channel_count(NULL)==6);assert(a->set_mask(NULL,0x25));assert(mock_levels[1] && mock_levels[41] && mock_levels[46]);assert(!a->set_channel(NULL,6,true));
    mock_write_fail=true;assert(!d->quiesce());assert(mock_pins[1]);mock_write_fail=false;
#elif TEST_ID==2
    const buzzer_api_v1 *a=d->capability;assert(a->chirp(NULL,0));assert(mock_wave_count==16);assert(!a->pattern(NULL,0,65535,65535,255));
#elif TEST_ID==3
    const button_api_v1 *a=d->capability;mock_levels[41]=false;a->poll(NULL,1000);a->poll(NULL,32000);assert(a->is_down(NULL,0));mock_levels[41]=true;a->poll(NULL,64000);a->poll(NULL,96000);assert(a->take_click(NULL,0));assert(!a->take_click(NULL,0));
#elif TEST_ID==4
    const led_api_v1 *a=d->capability;assert(a->set_duty(NULL,0,25));assert(mock_levels[40]);assert(!a->set_duty(NULL,0,101));
#elif TEST_ID==5
    const garden_encoder_api_v1 *a=d->capability;mock_levels[45]=mock_levels[42]=false;a->poll(NULL,1000);mock_levels[45]=mock_levels[42]=true;a->poll(NULL,2000);assert(a->take_detents(NULL)==1);assert(a->take_detents(NULL)==0);assert(a->button_pressed(NULL));assert(a->take_click(NULL));assert(a->take_long_press(NULL));
#elif TEST_ID==6
    const pixel_api_v1 *a=d->capability;assert(a->count(NULL)==5);assert(a->set_rgb(NULL,4,255,20,30));assert(a->set_brightness(NULL,50));assert(a->show(NULL));assert(mock_wave_count==240);assert(!a->set_rgb(NULL,5,1,2,3));
#elif TEST_ID==7 || TEST_ID==8
    const risc_display_output_api_v1 *a=d->capability;risc_display_info_v1 info;assert(a->get_info(NULL,&info));assert(info.width==(TEST_ID==7?240:800));
    risc_display_surface_v1 f;assert(a->acquire(NULL,info.preferred_format,&f));assert(!d->quiesce());
    /* quiesce revokes further submissions; release then restart cleanly. */
    a->release(NULL,f.frame);assert(d->quiesce());d->stop();assert(d->start(mock_deps,MOCK_N));
    assert(a->acquire(NULL,info.preferred_format,&f));memset(f.pixels,0x12,f.size_bytes);((uint8_t *)f.pixels)[0]=0x34;((uint8_t *)f.pixels)[f.size_bytes-1]=0xab;
    risc_display_present_token_v1 token;assert(!a->submit(NULL,f.frame+1,NULL,0,NULL,&token));assert(a->submit(NULL,f.frame,NULL,0,NULL,&token));assert(!a->acquire(NULL,info.preferred_format,&f));
    const risc_driver_poll_v2 *poller=(const void *)d;risc_display_present_status_v1 state;
    for(int i=0;i<600;i++){mock_now+=20;poller->poll(20);assert(a->present_status(NULL,token,&state));if(state.state==3)break;}
    assert(state.state==3);assert(mock_spi_bytes==(TEST_ID==7?115200:48000));
    if(TEST_ID==7){assert(mock_first[0]==0x12 && mock_first[1]==0x34);assert(mock_last[0]==0xab && mock_last[1]==0x12);}else{assert(mock_first[0]==(uint8_t)~0x34 && mock_last[1]==(uint8_t)~0xab);}
    assert(!a->present_status(NULL,token+1,&state));
#elif TEST_ID==9
    const risc_storage_volume_api_v1 *a=d->capability;assert(a->refresh(NULL));assert(a->ready(NULL));uint64_t size;bool dir;assert(a->stat(NULL,"/HELLO.TXT",&size,&dir)&&size==5&&!dir);
    risc_storage_file_t f=a->file_open_read(NULL,"/HELLO.TXT",&size);assert(f);char b[16]={0};assert(a->file_read(NULL,f,b,16)==5&&!strcmp(b,"hello"));assert(!d->quiesce());assert(a->file_close(NULL,f,true));assert(!a->file_read(NULL,f,b,16));
    f=a->file_open_write(NULL,"/new.txt");assert(f);assert(a->file_write(NULL,f,"garden",6)==6);assert(a->file_close(NULL,f,true));assert(!a->file_open_write(NULL,"/new.txt"));f=a->file_open_read(NULL,"/new.txt",&size);assert(f&&size==6);assert(a->file_read(NULL,f,b,16)==6&&!memcmp(b,"garden",6));assert(a->file_close(NULL,f,true));assert(a->remove(NULL,"/new.txt"));assert(!a->stat(NULL,"/new.txt",&size,&dir));
    f=a->file_open_write(NULL,"/abort.txt");assert(f);assert(a->file_write(NULL,f,"x",1)==1);assert(a->file_close(NULL,f,false));assert(!a->stat(NULL,"/abort.txt",&size,&dir));
#elif TEST_ID==10
    const risc_touch_api_v1 *a=d->capability;uint64_t sub=a->subscribe(NULL);assert(sub);risc_touch_snapshot_v1 snap;assert(a->snapshot(NULL,&snap)&&snap.contact_count==0);
    mock_touch[0]=1;mock_touch[2]=12;mock_touch[4]=34;assert(a->poll(NULL,1));risc_touch_event_v1 ev;assert(a->next(NULL,sub,&ev)==1&&ev.kind==RISC_TOUCH_EVENT_DOWN&&ev.x==12&&ev.y==34);
    for(int i=0;i<40;i++){mock_touch[2]++;assert(a->poll(NULL,1));}assert(a->next(NULL,sub,&ev)==-1);assert(a->snapshot(NULL,&snap)&&snap.contacts[0].x==52);mock_touch[0]=0;assert(a->poll(NULL,1));assert(a->next(NULL,sub,&ev)==1&&ev.kind==RISC_TOUCH_EVENT_UP);assert(!d->quiesce());assert(a->unsubscribe(NULL,sub));assert(a->next(NULL,sub,&ev)==-1);
#elif TEST_ID==11
    const wifi_api_v1 *a=d->capability;assert(a->connect(NULL,"garden","secret"));assert(a->status(NULL)==WIFI_LINK_JOINING);mock_radio_state=2;assert(a->status(NULL)==WIFI_LINK_UP&&a->rssi(NULL)==-42);assert(!a->connect(NULL,"","secret"));a->disconnect(NULL);assert(a->status(NULL)==WIFI_LINK_DOWN);wifi_ipv4_v1 cfg={{192,168,4,1},{192,168,4,1},{255,255,255,0}},sta,ap;assert(a->start_ap(NULL,"garden-ap","password",&cfg));assert(!a->start_ap(NULL,"garden-ap","short",&cfg));assert(a->addresses(NULL,&sta,&ap)&&ap.address[3]==1);assert(a->stop_ap(NULL));
#endif
#if TEST_ID!=12
    mock_release_fail=true;assert(!d->quiesce());mock_release_fail=false;
#endif
    assert(d->quiesce());d->stop();d->stop();for(int i=0;i<49;i++)assert(!mock_pins[i]);
#if TEST_ID!=11
    mock_board.board=99;assert(!d->start(mock_deps,MOCK_N));assert(d->quiesce());d->stop();mock_board.board=TEST_BOARD;
#endif
#if TEST_ID==1 || TEST_ID==5 || TEST_ID==7 || TEST_ID==8 || TEST_ID==10
    mock_fail_pin=TEST_ID==1?41:TEST_ID==5?42:TEST_ID==7?14:TEST_ID==8?6:5;
    assert(!d->start(mock_deps,MOCK_N));assert(d->quiesce());d->stop();
    for(int i=0;i<49;i++)assert(!mock_pins[i]);mock_fail_pin=-1;
#endif
    puts(DRIVER_SOURCE " contract fixture passed");return 0;
}
