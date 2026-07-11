#include <Arduino.h>
#include <Preferences.h>
#include "MasterSlaveSupport.h"

extern bool remoteEnabled;

static const char SYNC_PREF_NAMESPACE[] = "relay6";
static const char SYNC_ENABLED_KEY[] = "remoteEn";
static const char SYNC_INITIALIZED_KEY[] = "syncManaged";

// Called by the ESP32 Arduino core before setup().
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

  // Register role-aware routes and start the local master/slave service before
  // the sketch setup registers its legacy routes. WebServer resolves handlers
  // in registration order, so these role-aware handlers remain authoritative.
  meshEarlyInit();
}
