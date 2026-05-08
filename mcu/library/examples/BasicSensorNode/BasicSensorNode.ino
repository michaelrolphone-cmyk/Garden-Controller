#include <GardenDeviceFramework.h>
using namespace Garden;
class BasicSensorNode : public GardenDeviceBase {
public: explicit BasicSensorNode(const GardenDeviceOptions& options):GardenDeviceBase(options){}
protected:
  void appendCapabilities(JsonArray capabilities) override { capabilities.add("sensor.publisher"); capabilities.add("sensor.temperature"); }
  void appendDeviceState(JsonObject state) override { state["temperatureC"]=22.5; state["humidityPct"]=45.0; }
  void onDeviceLoop() override { static uint32_t last=0; if(millis()-last>60000UL){ last=millis(); JsonDocument doc; JsonObject payload=doc["payload"].to<JsonObject>(); payload["type"]="sensor.temperature"; payload["value"]=22.5; payload["unit"]="C"; enqueueEvent("sensor.reading",payload); } }
};
GardenDeviceOptions options = { .deviceType="sensor-node", .devicePrefix="GardenSensor", .firmwareVersion="sensor-example-0.1.0", .localPort=80, .defaultLocalApPassword="admin", .defaultAdminPassword="admin", .defaultMasterSsid="GardenMaster", .defaultMasterPassword="admin", .defaultMasterBaseUrl="http://garden-master.local" };
BasicSensorNode node(options);
void setup(){ node.begin(); }
void loop(){ node.loop(); }
