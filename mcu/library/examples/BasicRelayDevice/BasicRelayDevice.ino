#include <GardenDeviceFramework.h>
#include <GardenRelayDevice.h>
using namespace Garden;
const uint8_t RELAY_PINS[] = {1, 2, 41, 42, 45, 46};
GardenDeviceOptions options = {
  .deviceType = "relay-controller",
  .devicePrefix = "GardenRelay",
  .firmwareVersion = "relay-example-0.1.0",
  .localPort = 80,
  .defaultLocalApPassword = "change-me",
  .defaultAdminPassword = "change-me",
  .defaultMasterSsid = "GardenMaster",
  .defaultMasterPassword = "change-me",
  .defaultMasterBaseUrl = "http://garden-master.local"
};
GardenRelayDevice relay(options, RELAY_PINS, 5, 6);
void setup(){ relay.setRelayActiveLevel(false); relay.begin(); }
void loop(){ relay.loop(); }
