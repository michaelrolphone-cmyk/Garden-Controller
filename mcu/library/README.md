# GardenDeviceFramework

Arduino/ESP32 base framework for the garden control mesh.

Provides a common foundation for relay controllers, display controllers, sensor bridges, sensor nodes, rotary encoder panels, camera nodes, and other garden devices.

## Core features

- Persistent unique device ID generated on first boot and stored until factory reset.
- Dual network mode: connects to the master display WiFi while exposing its own fallback AP/admin UI.
- Local AP SSID is named from the device prefix and ID.
- Requested default AP password is `admin`; ESP32 WPA2 requires 8+ characters, so this library opens the AP if the password remains shorter than 8 chars. Set a longer password in local admin for WPA2.
- Local web UI and JSON endpoints.
- Master sync client for the paper-display SD-card coordinator.
- Dirty local config tracking so direct device edits are pushed to the master before overwrite.
- Config version/hash and conflict guard.
- Typed message/event protocol.
- Event outbox.
- Extensible virtual hooks.

## Local endpoints

```text
GET  /
GET  /api/state
GET  /api/config
POST /api/config
POST /api/network
POST /api/sync/push
POST /api/sync/pull
POST /api/factory-reset
```

Relay helper endpoints:

```text
GET /api/zone/run?zone=1&seconds=900
GET /api/zone/stop?zone=1
GET /api/alloff
```

## Master endpoints expected

The paper display master should implement:

```text
POST /api/master/devices/{deviceId}/heartbeat
GET  /api/master/devices/{deviceId}/messages/next?wait=25
POST /api/master/messages/{messageId}/ack
POST /api/master/devices/{deviceId}/events
POST /api/master/devices/{deviceId}/config/local-change
```

## Extend the base

```cpp
class MyDevice : public Garden::GardenDeviceBase {
protected:
  void appendCapabilities(JsonArray capabilities) override;
  void appendDeviceState(JsonObject state) override;
  void appendDeviceConfig(JsonObject config) override;
  bool applyDeviceConfig(JsonObjectConst config) override;
  bool handleDeviceMessage(const String& type, JsonObjectConst message, JsonObjectConst payload) override;
};
```

## Examples

- `BasicRelayDevice`
- `BasicSensorNode`


## 0.1.1

Changed default local AP/admin password from `admin` to `change-me`.

ESP32 WPA2 AP mode requires an 8+ character password, so `change-me` works as a real WPA2 AP password instead of forcing an open AP.
