#include "dependency.h"
static bool active;
static const garden_board_v1 profile={1,sizeof(profile),BOARD_ID};
static bool start(const risc_provider_dependency_v1 *d,size_t n) {
    const garden_board_v1 *actual=garden_dependency(d,n,"platform.board",sizeof(*actual));
    if (active || !actual || actual->board!=BOARD_ID) return false;
    active=true; return true;
}
static bool quiesce(void) { active=false; return true; }
static void stop(void) { active=false; }
static const risc_driver_v2 driver={2,sizeof(driver),DRIVER_ID,"board.garden",1,&profile,start,stop,quiesce};
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi==2?&driver:NULL; }
