#include "dependency.h"
#include "RiscHardwareConfigV1.h"
static bool active;
static const risc_hardware_catalog_v1 catalog={1,sizeof(catalog),BOARD_ID,BOARD_REVISION,board_json,sizeof(board_json)-1};
static bool start(const risc_provider_dependency_v1 *d,size_t n) {
    const risc_hardware_board_identity_v2 *actual=NULL;
    if(active || !d || n>16) return false;
    for(size_t i=0;i<n;i++) if(d[i].capability_id && !strcmp(d[i].capability_id,"platform.board")) {
        if(actual || d[i].api_version!=2 || !d[i].api) return false;
        actual=d[i].api;
    }
    if(!actual || actual->api_version!=2 || actual->struct_size<sizeof(*actual) || !actual->board_id || !actual->revision ||
       strcmp(actual->board_id,BOARD_ID) || strcmp(actual->revision,BOARD_REVISION)) return false;
    active=true;return true;
}
static bool quiesce(void) { active=false;return true; }
static void stop(void) { active=false; }
static const risc_driver_v2 driver={2,sizeof(driver),DRIVER_ID,"hardware.catalog",1,&catalog,start,stop,quiesce};
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi==2?&driver:NULL; }
