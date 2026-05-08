#pragma once
#include "GardenDeviceFramework.h"
namespace Garden {
struct RelayZone { uint8_t channel=0; bool enabled=true; char name[24]={0}; };
struct RelayRun { bool active=false; uint32_t startedMs=0; uint32_t durationMs=0; };
class GardenRelayDevice : public GardenDeviceBase {
public:
  GardenRelayDevice(const GardenDeviceOptions& options,const uint8_t* relayPins,uint8_t zoneCount,uint8_t masterChannel);
  void setRelayActiveLevel(bool activeHigh);
  void allOff();
  bool startZone(uint8_t zone,uint32_t durationSeconds);
  bool stopZone(uint8_t zone);
protected:
  void onDeviceBegin() override;
  void onDeviceLoop() override;
  void appendCapabilities(JsonArray capabilities) override;
  void appendDeviceState(JsonObject state) override;
  void appendDeviceConfig(JsonObject config) override;
  bool applyDeviceConfig(JsonObjectConst config) override;
  bool handleDeviceMessage(const String& type,JsonObjectConst message,JsonObjectConst payload) override;
private:
  static const uint8_t MAX_ZONES=16;
  const uint8_t* _relayPins;
  uint8_t _zoneCount;
  uint8_t _masterChannel;
  bool _activeHigh=false;
  RelayZone _zones[MAX_ZONES];
  RelayRun _runs[MAX_ZONES];
  bool _relayStates[MAX_ZONES+1];
  void writeRelay(uint8_t channel,bool on);
  bool anyZoneActive() const;
  void updateMaster();
  void setupRelayRoutes();
  void handleRunRoute();
  void handleStopRoute();
};
}
