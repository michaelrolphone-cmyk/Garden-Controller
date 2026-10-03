#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RISC_RUNTIME_API_V1 1u
/* Minimal headless runtime service. Append-only. Available only on the active
 * app owner task, from module init through fini. Native apps are trusted code.
 * No pointers/callbacks/tasks may outlive app_main/fini. */
typedef struct {
  uint32_t struct_size;
  uint32_t uptime_ms, free_heap, app_address;
  uint8_t mac[6];
  char target[96];
} risc_runtime_health_v1;
/* App-scoped opaque grant. Zero-initialize and set struct_size before acquire.
 * The interface has its capability's canonical version/size header. Consumers
 * must check that header against the complete table they use. */
typedef struct {
  uint32_t struct_size, slot, generation;
  const void* api;
} risc_runtime_capability_v1;
typedef struct {
  uint32_t api_version, struct_size;
  bool (*health)(risc_runtime_health_v1* out);
  void (*yield_ms)(uint32_t milliseconds); /* clamp 1..50; polls providers */
  bool (*diagnostic)(const char* line); /* one-way, max 255 bytes, newline added */
  /* Copies one normalized relative .elf path under the configured boot store.
   * True queues a handoff: return from app_main immediately. Current ELF is
   * finalized and unmapped before the next load. A second request is denied.
   * Failure to unload/quiesce blocks handoff. Child return/load failure reloads the configured default from scratch.
   * Default return enters Idle; default failure enters Error. */
  bool (*request_launch)(const char* relative_elf);
  /* Append-only capability extension. Allowed only by the active app manifest
   * and boot grant policy. instance_id=0 requires a unique authorized provider;
   * nonzero selects that exact hardware instance. No registry-order fallback.
   * Release frames/sessions/operations through their capability before release
   * or app return. Remaining grants are revoked before app memory/image teardown;
   * all pointers become invalid on release or end of this app invocation. */
  bool (*acquire)(const char* capability, uint32_t version, uint64_t instance_id,
                  risc_runtime_capability_v1* out);
  bool (*release)(risc_runtime_capability_v1* grant);
} risc_runtime_api_v1;
#define RISC_RUNTIME_CAPABILITIES_V1_SIZE (offsetof(risc_runtime_api_v1, release) + sizeof(((risc_runtime_api_v1*)0)->release))
const risc_runtime_api_v1* risc_runtime_get_api(uint32_t version);
#ifdef __cplusplus
}
#endif
