#include <Arduino.h>
#include <Preferences.h>
#include "MasterSlaveSupport.h"

extern bool remoteEnabled;

static const char SYNC_PREF_NAMESPACE[] = "relay6";
static const char SYNC_ENABLED_KEY[] = "remoteEn";
static const char SYNC_INITIALIZED_KEY[] = "syncManaged";

// Called by the ESP32 Arduino core before setup(). This phase is intentionally
// limited to persisted safety settings, route precedence, and role information.
// No relay, WiFi, heartbeat, scheduler, or time-service task starts here.
void initVariant() {
  remoteEnabled = false;

  Preferences settings;
  if (settings.begin(SYNC_PREF_NAMESPACE, false)) {
    if (!settings.getBool(SYNC_INITIALIZED_KEY, false)) {
      settings.putBool(SYNC_ENABLED_KEY, false);
      settings.putBool(SYNC_INITIALIZED_KEY, true);
    }

    remoteEnabled = settings.getBool(SYNC_ENABLED_KEY, false);
    settings.end();
  }

  // Authoritative routes must still be inserted before setupServer() registers
  // the compatibility routes. The mesh role is also needed before setupAp() so
  // a slave uses its non-conflicting recovery subnet.
  slaveInternetTimePreInit();
  meshReliablePreInit();
  meshPreSetupInitNoTask();

  // A passive coordinator waits for the normal sketch setup to finish before
  // starting any service that can touch relays, WiFi, schedules, or the clock.
  gardenArmPostSetupServices();
}
