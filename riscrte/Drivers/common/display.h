/* Shared canonical display.output@1 provider. Included by a controller ELF.
 * Controller defines WIDTH/HEIGHT/FORMAT/STRIDE/DC and hw_start/hw_row/hw_finish.
 * Single retained frame; accepted submit consumes the acquire lease. */
#include "RiscDisplayOutputV1.h"
#include <stdatomic.h>
static atomic_flag display_lock=ATOMIC_FLAG_INIT;
static atomic_bool release_pending;
static bool running,held,active,failed,closing;
static uint64_t frame_serial,present_serial;
static uint8_t present_state;
static uint32_t next_row;
static uint8_t pixels[STRIDE*HEIGHT] __attribute__((aligned(4)));
static bool enter(void) {
    if (atomic_flag_test_and_set_explicit(&display_lock,memory_order_acquire)) return false;
    if (atomic_exchange(&release_pending,false)) held=false;
    return true;
}
static void leave(void) { atomic_flag_clear_explicit(&display_lock,memory_order_release); }
static bool get_info(void *c,risc_display_info_v1 *out) {
    (void)c; if (!out || !enter()) return false;
    bool ok=running && !closing;
    if (ok) *out=(risc_display_info_v1){.api_version=1,.struct_size=sizeof(*out),.width=WIDTH,.height=HEIGHT,
        .supported_formats=RISC_DISPLAY_FORMAT_BIT(FORMAT),.preferred_format=FORMAT,
        .supported_rotations=RISC_DISPLAY_ROTATION_0,.flags=DISPLAY_FLAGS,
        .damage_x_alignment=1,.damage_y_alignment=1,.damage_width_alignment=1,.damage_height_alignment=1};
    leave(); return ok;
}
static bool acquire(void *c,uint32_t format,risc_display_surface_v1 *out) {
    (void)c; if (!out || format!=FORMAT || !enter()) return false;
    bool ok=running && !closing && !held && !active && !failed && frame_serial!=UINT64_MAX;
    if (ok) { held=true; ++frame_serial; *out=(risc_display_surface_v1){frame_serial,pixels,WIDTH,HEIGHT,STRIDE,sizeof(pixels),FORMAT}; }
    leave(); return ok;
}
static void release(void *c,risc_display_frame_v1 frame) {
    (void)c;
    /* API client serializes frame ownership. Poll never alters frame_serial. */
    if (frame==frame_serial) atomic_store(&release_pending,true);
}
static bool submit(void *c,risc_display_frame_v1 frame,const risc_display_rect_v1 *damage,size_t count,
                   const risc_display_present_options_v1 *options,risc_display_present_token_v1 *out) {
    (void)c;
    if (count>8 || (count && !damage) || (options && (options->reserved || options->intent>3 || options->queue_policy!=RISC_DISPLAY_QUEUE_FIFO))) return false;
    for (size_t i=0;i<count;i++) {
        const risc_display_rect_v1 *r=&damage[i];
        if (r->x<0 || r->y<0 || !r->width || !r->height || (uint32_t)r->x>=WIDTH || (uint32_t)r->y>=HEIGHT || r->width>WIDTH-(uint32_t)r->x || r->height>HEIGHT-(uint32_t)r->y) return false;
    }
    if (!enter()) return false;
    bool ok=running && !closing && !failed && held && frame==frame_serial && !active && present_serial!=UINT64_MAX;
    if (ok) { held=false; active=true; next_row=0; present_state=RISC_DISPLAY_PRESENT_QUEUED; ++present_serial; if (out) *out=present_serial; hw_submit(); }
    leave(); return ok;
}
static bool present_status(void *c,risc_display_present_token_v1 token,risc_display_present_status_v1 *out) {
    (void)c; if (!out || !enter()) return false;
    bool ok=running && token && token==present_serial;
    if (ok) *out=(risc_display_present_status_v1){.state=present_state};
    leave(); return ok;
}
static void display_poll(uint32_t budget_ms) {
    if (!budget_ms || !enter()) return;
    spi_timeout_ms=budget_ms<20?budget_ms:20;
    if (running && active && !failed) {
        present_state=RISC_DISPLAY_PRESENT_ACTIVE;
        if (next_row<HEIGHT) {
            if (hw_row(next_row,pixels+next_row*STRIDE)) ++next_row;
            else { failed=true; active=false; present_state=RISC_DISPLAY_PRESENT_FAILED; }
        } else {
            int done=hw_finish();
            if (done) { active=false; failed=done<0; present_state=failed?RISC_DISPLAY_PRESENT_FAILED:RISC_DISPLAY_PRESENT_COMPLETE; }
        }
    }
    leave();
}
static bool wait_present(void *c,risc_display_present_token_v1 token,uint32_t timeout,risc_display_present_status_v1 *out) {
    if (!timer) return false;
    uint32_t budget=timeout>250?250:timeout;
    uint64_t began=timer->monotonic_ms(timer->context);
    for (uint32_t steps=0;;steps++) {
        if (!present_status(c,token,out)) return false;
        if (out->state>=RISC_DISPLAY_PRESENT_COMPLETE || !budget || steps>=budget || timer->monotonic_ms(timer->context)-began>=budget) return true;
        timer->sleep_ms(timer->context,1);
    }
}
static bool set_brightness(void *c,uint16_t level,uint16_t maximum) {
    (void)c; if (!maximum || level>maximum || !enter()) return false;
    bool ok=running && !closing && hw_brightness(level,maximum); leave(); return ok;
}
static bool quiesce(void) {
    if (!enter()) return false;
    closing=true;
    bool ok=!held && !active && hw_stop();
    if (ok) running=false;
    leave(); return ok;
}
static void stop(void) { (void)quiesce(); }
static bool start(const risc_provider_dependency_v1 *d,size_t n) {
    if (running || !hw_start(d,n)) return false;
    closing=failed=held=active=false; atomic_store(&release_pending,false);
    memset(pixels,0,sizeof(pixels)); running=true; return true;
}
static const risc_display_output_api_v1 api={1,sizeof(api),NULL,get_info,acquire,release,submit,present_status,wait_present,set_brightness};
static const risc_driver_poll_v2 descriptor={
    .streams={.driver={2,sizeof(descriptor),DISPLAY_ID,"display.output",1,&api,start,stop,quiesce}},.poll=display_poll
};
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi==2?&descriptor.streams.driver:NULL; }
