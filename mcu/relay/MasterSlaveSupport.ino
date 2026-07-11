#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// One firmware image supports two roles:
//   MASTER: normal five-zone + spigot controller and coordinator for slaves.
//   SLAVE: six independent timed relay outputs controlled by the master.
//
// Master logical channel allocation intentionally preserves channel 6 as the
// existing spigot/master-valve resource:
//   1-5  local master zones
//   6    local master spigots
//   7-12 first slave relays 1-6
//   13-18 second slave relays 1-6, etc.

enum MeshRole : uint8_t {
  MESH_ROLE_MASTER = 0,
  MESH_ROLE_SLAVE = 1
};

static const uint8_t MESH_MAX_SLAVES = 4;
static const uint8_t MESH_RELAYS_PER_SLAVE = 6;
static const uint8_t MESH_FIRST_LOGICAL_CHANNEL = 7;
static const uint8_t MESH_MAX_REMOTE_SCHEDULES = 64;
static const uint32_t MESH_HEARTBEAT_INTERVAL_MS = 5000UL;
static const uint32_t MESH_SLAVE_OFFLINE_MS = 15000UL;
static const uint32_t MESH_COMMAND_TIMEOUT_MS = 2500UL;
static const char MESH_PREF_NAMESPACE[] = "relay6mesh";

struct MeshSlaveDevice {
  bool assigned;
  char id[32];
  char name[32];
  char ip[16];
  uint8_t slot;
  uint32_t lastSeenMs;
  bool relayOn[MESH_RELAYS_PER_SLAVE];
  uint32_t remainingSeconds[MESH_RELAYS_PER_SLAVE];
};

struct MeshRemoteSchedule {
  bool enabled;
  uint8_t logicalChannel;
  uint8_t startHour;
  uint8_t startMinute;
  uint16_t runMinutes;
  uint8_t daysMask;
  int lastRunYearDay;
  int lastRunMinuteOfDay;
};

struct MeshSlaveRun {
  bool active;
  uint32_t startedMs;
  uint32_t durationMs;
};

static MeshRole meshRole = MESH_ROLE_MASTER;
static char meshDeviceId[32] = "";
static char meshDeviceName[32] = "Garden Controller";
static char meshMasterHost[48] = "192.168.4.1";
static char meshLinkKey[64] = "gardenwater";
static MeshSlaveDevice meshSlaves[MESH_MAX_SLAVES];
static MeshRemoteSchedule meshSchedules[MESH_MAX_REMOTE_SCHEDULES];
static uint8_t meshScheduleCount = 0;
static MeshSlaveRun meshSlaveRuns[MESH_RELAYS_PER_SLAVE];
static SemaphoreHandle_t meshMutex = nullptr;
static TaskHandle_t meshTaskHandle = nullptr;
static bool meshLegacyNeutralized = false;
static uint32_t meshLastHeartbeatMs = 0;
static uint32_t meshLastMasterContactMs = 0;
static int meshAssignedSlot = -1;
static int meshAssignedBaseChannel = -1;
static String meshLastStatus = "mesh starting";

class MeshGuard {
 public:
  bool acquired;
  MeshGuard()
      : acquired(
            meshMutex == nullptr ||
            xSemaphoreTakeRecursive(meshMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {}
  ~MeshGuard() {
    if (meshMutex != nullptr && acquired) xSemaphoreGiveRecursive(meshMutex);
  }
};

static String meshRoleName() {
  return meshRole == MESH_ROLE_SLAVE ? "slave" : "master";
}

static bool meshIsMaster() {
  return meshRole == MESH_ROLE_MASTER;
}

static bool meshIsSlave() {
  return meshRole == MESH_ROLE_SLAVE;
}

static uint8_t meshMaxLogicalChannel() {
  return MESH_FIRST_LOGICAL_CHANNEL + MESH_MAX_SLAVES * MESH_RELAYS_PER_SLAVE - 1;
}

static uint8_t meshLogicalChannel(uint8_t slot, uint8_t relayIndex) {
  return MESH_FIRST_LOGICAL_CHANNEL + slot * MESH_RELAYS_PER_SLAVE + relayIndex;
}

static bool meshDecodeLogicalChannel(int logicalChannel, uint8_t& slotOut, uint8_t& relayIndexOut) {
  if (logicalChannel < MESH_FIRST_LOGICAL_CHANNEL || logicalChannel > meshMaxLogicalChannel()) return false;
  uint8_t offset = (uint8_t)(logicalChannel - MESH_FIRST_LOGICAL_CHANNEL);
  slotOut = offset / MESH_RELAYS_PER_SLAVE;
  relayIndexOut = offset % MESH_RELAYS_PER_SLAVE;
  return slotOut < MESH_MAX_SLAVES;
}

static bool meshSlaveOnline(const MeshSlaveDevice& slave) {
  return slave.assigned && slave.lastSeenMs > 0 && millis() - slave.lastSeenMs <= MESH_SLAVE_OFFLINE_MS;
}

static void meshGenerateDefaultIdentity() {
  if (strlen(meshDeviceId) > 0) return;
  uint32_t suffix = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFULL);
  snprintf(meshDeviceId, sizeof(meshDeviceId), "relay-%06lX", (unsigned long)suffix);
  snprintf(meshDeviceName, sizeof(meshDeviceName), "Garden Relay %06lX", (unsigned long)suffix);
}

static void meshClearRuntimeArrays() {
  for (uint8_t i = 0; i < MESH_MAX_SLAVES; i++) {
    meshSlaves[i].assigned = false;
    meshSlaves[i].id[0] = 0;
    meshSlaves[i].name[0] = 0;
    meshSlaves[i].ip[0] = 0;
    meshSlaves[i].slot = i;
    meshSlaves[i].lastSeenMs = 0;
    for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
      meshSlaves[i].relayOn[relay] = false;
      meshSlaves[i].remainingSeconds[relay] = 0;
    }
  }

  meshScheduleCount = 0;
  for (uint8_t i = 0; i < MESH_MAX_REMOTE_SCHEDULES; i++) {
    meshSchedules[i].enabled = false;
    meshSchedules[i].logicalChannel = MESH_FIRST_LOGICAL_CHANNEL;
    meshSchedules[i].startHour = 0;
    meshSchedules[i].startMinute = 0;
    meshSchedules[i].runMinutes = 0;
    meshSchedules[i].daysMask = 0x7F;
    meshSchedules[i].lastRunYearDay = -1;
    meshSchedules[i].lastRunMinuteOfDay = -1;
  }

  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    meshSlaveRuns[relay].active = false;
    meshSlaveRuns[relay].startedMs = 0;
    meshSlaveRuns[relay].durationMs = 0;
  }
}

static void meshSavePreferences() {
  MeshGuard guard;
  if (!guard.acquired) return;

  Preferences p;
  if (!p.begin(MESH_PREF_NAMESPACE, false)) return;
  p.putUChar("role", (uint8_t)meshRole);
  p.putString("deviceId", meshDeviceId);
  p.putString("deviceName", meshDeviceName);
  p.putString("masterHost", meshMasterHost);
  p.putString("linkKey", meshLinkKey);

  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    String key = "slot" + String(slot);
    p.putString((key + "id").c_str(), meshSlaves[slot].assigned ? meshSlaves[slot].id : "");
    p.putString((key + "name").c_str(), meshSlaves[slot].assigned ? meshSlaves[slot].name : "");
  }

  p.putUChar("schedCount", meshScheduleCount);
  for (uint8_t i = 0; i < meshScheduleCount; i++) {
    String key = "r" + String(i);
    p.putBool((key + "en").c_str(), meshSchedules[i].enabled);
    p.putUChar((key + "ch").c_str(), meshSchedules[i].logicalChannel);
    p.putUChar((key + "h").c_str(), meshSchedules[i].startHour);
    p.putUChar((key + "m").c_str(), meshSchedules[i].startMinute);
    p.putUShort((key + "dur").c_str(), meshSchedules[i].runMinutes);
    p.putUChar((key + "mask").c_str(), meshSchedules[i].daysMask & 0x7F);
  }
  p.end();
}

static void meshLoadPreferences() {
  meshClearRuntimeArrays();

  Preferences p;
  if (p.begin(MESH_PREF_NAMESPACE, true)) {
    meshRole = p.getUChar("role", MESH_ROLE_MASTER) == MESH_ROLE_SLAVE
        ? MESH_ROLE_SLAVE
        : MESH_ROLE_MASTER;

    String value = p.getString("deviceId", "");
    strlcpy(meshDeviceId, value.c_str(), sizeof(meshDeviceId));
    value = p.getString("deviceName", "Garden Controller");
    strlcpy(meshDeviceName, value.c_str(), sizeof(meshDeviceName));
    value = p.getString("masterHost", "192.168.4.1");
    strlcpy(meshMasterHost, value.c_str(), sizeof(meshMasterHost));
    value = p.getString("linkKey", "gardenwater");
    strlcpy(meshLinkKey, value.c_str(), sizeof(meshLinkKey));

    for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
      String key = "slot" + String(slot);
      value = p.getString((key + "id").c_str(), "");
      if (value.length()) {
        meshSlaves[slot].assigned = true;
        strlcpy(meshSlaves[slot].id, value.c_str(), sizeof(meshSlaves[slot].id));
        value = p.getString((key + "name").c_str(), meshSlaves[slot].id);
        strlcpy(meshSlaves[slot].name, value.c_str(), sizeof(meshSlaves[slot].name));
      }
    }

    uint8_t savedCount = min((uint8_t)MESH_MAX_REMOTE_SCHEDULES, p.getUChar("schedCount", 0));
    for (uint8_t i = 0; i < savedCount; i++) {
      String key = "r" + String(i);
      uint8_t channel = p.getUChar((key + "ch").c_str(), 0);
      uint8_t hour = p.getUChar((key + "h").c_str(), 0);
      uint8_t minute = p.getUChar((key + "m").c_str(), 0);
      uint16_t duration = p.getUShort((key + "dur").c_str(), 0);
      if (channel < MESH_FIRST_LOGICAL_CHANNEL || channel > meshMaxLogicalChannel() || hour > 23 || minute > 59 || duration == 0) continue;

      MeshRemoteSchedule& schedule = meshSchedules[meshScheduleCount++];
      schedule.enabled = p.getBool((key + "en").c_str(), true);
      schedule.logicalChannel = channel;
      schedule.startHour = hour;
      schedule.startMinute = minute;
      schedule.runMinutes = constrain(duration, 1, 240);
      schedule.daysMask = p.getUChar((key + "mask").c_str(), 0x7F) & 0x7F;
      schedule.lastRunYearDay = -1;
      schedule.lastRunMinuteOfDay = -1;
    }
    p.end();
  }

  meshGenerateDefaultIdentity();
  if (strlen(meshMasterHost) == 0) strlcpy(meshMasterHost, "192.168.4.1", sizeof(meshMasterHost));
  if (strlen(meshLinkKey) == 0) strlcpy(meshLinkKey, "gardenwater", sizeof(meshLinkKey));
}

static void meshSetSlaveApSubnet() {
  apIp = IPAddress(192, 168, 5, 1);
  apGw = IPAddress(192, 168, 5, 1);
  apMask = IPAddress(255, 255, 255, 0);
}

static void meshStopAllSlaveRelays() {
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    meshSlaveRuns[relay].active = false;
    setRelay(relay, false);
  }
}

static void meshNeutralizeLegacyControllerForSlave() {
  if (!meshIsSlave() || meshLegacyNeutralized) return;

  dailyScheduleCount = 0;
  trackedScheduleCount = 0;
  spigotScheduleCount = 0;
  spigotRun.active = false;
  for (uint8_t zone = 0; zone < ZONE_COUNT; zone++) zoneRuns[zone].active = false;
  meshStopAllSlaveRelays();
  meshLegacyNeutralized = true;
  meshLastStatus = "slave mode ready; waiting for master";
}

static void meshStartSlaveRelay(uint8_t relayIndex, uint32_t durationSeconds) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE) return;
  durationSeconds = constrain(durationSeconds, 1UL, 240UL * 60UL);
  meshSlaveRuns[relayIndex].active = true;
  meshSlaveRuns[relayIndex].startedMs = millis();
  meshSlaveRuns[relayIndex].durationMs = durationSeconds * 1000UL;
  setRelay(relayIndex, true);
}

static void meshStopSlaveRelay(uint8_t relayIndex) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE) return;
  meshSlaveRuns[relayIndex].active = false;
  setRelay(relayIndex, false);
}

static uint32_t meshSlaveRemainingSeconds(uint8_t relayIndex) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE || !meshSlaveRuns[relayIndex].active) return 0;
  uint32_t elapsed = millis() - meshSlaveRuns[relayIndex].startedMs;
  if (elapsed >= meshSlaveRuns[relayIndex].durationMs) return 0;
  return (meshSlaveRuns[relayIndex].durationMs - elapsed + 999UL) / 1000UL;
}

static void meshServiceSlaveTimers() {
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    if (!meshSlaveRuns[relay].active) continue;
    if (millis() - meshSlaveRuns[relay].startedMs >= meshSlaveRuns[relay].durationMs) {
      meshStopSlaveRelay(relay);
    }
  }
}

static String meshMasterBaseUrl() {
  String base = String(meshMasterHost);
  base.trim();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!base.startsWith("http://") && !base.startsWith("https://")) base = "http://" + base;
  return base;
}

static bool meshPostJson(const String& url, const String& body, String& responseOut, int& codeOut) {
  responseOut = "";
  codeOut = -1;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(MESH_COMMAND_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  codeOut = http.POST(body);
  if (codeOut > 0) responseOut = http.getString();
  http.end();
  return codeOut >= 200 && codeOut < 300;
}

static int meshFindOrAssignSlave(const char* id, const char* name) {
  if (!id || !strlen(id)) return -1;

  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    if (meshSlaves[slot].assigned && strcmp(meshSlaves[slot].id, id) == 0) {
      if (name && strlen(name)) strlcpy(meshSlaves[slot].name, name, sizeof(meshSlaves[slot].name));
      return slot;
    }
  }

  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    if (meshSlaves[slot].assigned) continue;
    meshSlaves[slot].assigned = true;
    meshSlaves[slot].slot = slot;
    strlcpy(meshSlaves[slot].id, id, sizeof(meshSlaves[slot].id));
    strlcpy(meshSlaves[slot].name, name && strlen(name) ? name : id, sizeof(meshSlaves[slot].name));
    meshSavePreferences();
    return slot;
  }

  return -1;
}

static bool meshVerifyLinkKey(JsonDocument& doc) {
  const char* supplied = doc["linkKey"] | "";
  return strlen(meshLinkKey) > 0 && strcmp(supplied, meshLinkKey) == 0;
}

static void meshHandleSlaveHeartbeat() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in master mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, server.arg("plain")) || !meshVerifyLinkKey(doc)) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid slave registration\"}");
    return;
  }

  const char* id = doc["deviceId"] | "";
  const char* name = doc["deviceName"] | id;
  MeshGuard guard;
  if (!guard.acquired) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"mesh busy\"}");
    return;
  }

  int slot = meshFindOrAssignSlave(id, name);
  if (slot < 0) {
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"no slave slots available\"}");
    return;
  }

  MeshSlaveDevice& slave = meshSlaves[slot];
  String remoteIp = server.client().remoteIP().toString();
  strlcpy(slave.ip, remoteIp.c_str(), sizeof(slave.ip));
  slave.lastSeenMs = millis();

  JsonArray relays = doc["relays"].as<JsonArray>();
  for (JsonObject relay : relays) {
    int number = relay["relay"] | 0;
    if (number < 1 || number > MESH_RELAYS_PER_SLAVE) continue;
    slave.relayOn[number - 1] = relay["on"] | false;
    slave.remainingSeconds[number - 1] = relay["remainingSeconds"] | 0;
  }

  DynamicJsonDocument response(512);
  response["ok"] = true;
  response["slot"] = slot;
  response["baseLogicalChannel"] = meshLogicalChannel(slot, 0);
  response["heartbeatIntervalSeconds"] = MESH_HEARTBEAT_INTERVAL_MS / 1000UL;
  response["masterTime"] = (uint32_t)time(nullptr);
  String body;
  serializeJson(response, body);
  server.send(200, "application/json", body);
}

static bool meshSendSlaveCommand(uint8_t logicalChannel, const String& action, uint32_t durationSeconds) {
  uint8_t slot = 0;
  uint8_t relayIndex = 0;
  if (!meshDecodeLogicalChannel(logicalChannel, slot, relayIndex)) return false;

  char ip[16];
  bool online = false;
  {
    MeshGuard guard;
    if (!guard.acquired) return false;
    MeshSlaveDevice& slave = meshSlaves[slot];
    online = meshSlaveOnline(slave);
    strlcpy(ip, slave.ip, sizeof(ip));
  }

  if (!online || !strlen(ip)) {
    meshLastStatus = "remote zone command skipped: slave offline";
    return false;
  }

  DynamicJsonDocument doc(512);
  doc["linkKey"] = meshLinkKey;
  doc["relay"] = relayIndex + 1;
  doc["action"] = action;
  doc["durationSeconds"] = constrain(durationSeconds, 1UL, 240UL * 60UL);
  doc["logicalChannel"] = logicalChannel;
  String body;
  serializeJson(doc, body);

  String response;
  int code = -1;
  bool ok = meshPostJson("http://" + String(ip) + "/api/slave/relay", body, response, code);
  meshLastStatus = String("slave command channel ") + logicalChannel + " -> " + code;
  return ok;
}

static void meshHandleSlaveRelayCommand() {
  if (!meshIsSlave()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in slave mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, server.arg("plain")) || !meshVerifyLinkKey(doc)) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid master command\"}");
    return;
  }

  int relay = doc["relay"] | 0;
  String action = String((const char*)(doc["action"] | ""));
  uint32_t durationSeconds = doc["durationSeconds"] | DEFAULT_MANUAL_RUN_SECONDS;
  if (relay < 1 || relay > MESH_RELAYS_PER_SLAVE || (action != "on" && action != "off" && action != "toggle")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid relay command\"}");
    return;
  }

  uint8_t relayIndex = relay - 1;
  if (action == "off") {
    meshStopSlaveRelay(relayIndex);
  } else if (action == "toggle" && relayState[relayIndex]) {
    meshStopSlaveRelay(relayIndex);
  } else {
    meshStartSlaveRelay(relayIndex, durationSeconds);
  }
  meshLastMasterContactMs = millis();
  meshLastStatus = String("master command applied to relay ") + relay;

  DynamicJsonDocument response(384);
  response["ok"] = true;
  response["relay"] = relay;
  response["on"] = relayState[relayIndex];
  response["remainingSeconds"] = meshSlaveRemainingSeconds(relayIndex);
  String body;
  serializeJson(response, body);
  server.send(200, "application/json", body);
}

static void meshHandleSlaveAllOff() {
  if (!meshIsSlave()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in slave mode\"}");
    return;
  }
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain")) || !meshVerifyLinkKey(doc)) {
      server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid master command\"}");
      return;
    }
  }
  meshStopAllSlaveRelays();
  server.send(200, "application/json", "{\"ok\":true}");
}

static bool meshSendHeartbeat() {
  if (!meshIsSlave() || WiFi.status() != WL_CONNECTED) return false;

  DynamicJsonDocument doc(2048);
  doc["linkKey"] = meshLinkKey;
  doc["deviceId"] = meshDeviceId;
  doc["deviceName"] = meshDeviceName;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["relayCount"] = MESH_RELAYS_PER_SLAVE;
  doc["stationIp"] = WiFi.localIP().toString();
  JsonArray relays = doc.createNestedArray("relays");
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    JsonObject item = relays.createNestedObject();
    item["relay"] = relay + 1;
    item["on"] = relayState[relay];
    item["remainingSeconds"] = meshSlaveRemainingSeconds(relay);
  }

  String body;
  serializeJson(doc, body);
  String response;
  int code = -1;
  bool ok = meshPostJson(meshMasterBaseUrl() + "/api/slaves/heartbeat", body, response, code);
  if (!ok) {
    meshLastStatus = String("master heartbeat failed: ") + code;
    return false;
  }

  DynamicJsonDocument reply(768);
  if (deserializeJson(reply, response)) {
    meshLastStatus = "master heartbeat response invalid";
    return false;
  }

  meshAssignedSlot = reply["slot"] | -1;
  meshAssignedBaseChannel = reply["baseLogicalChannel"] | -1;
  meshLastMasterContactMs = millis();
  meshLastStatus = String("connected to master as zones ") + meshAssignedBaseChannel + "-" + (meshAssignedBaseChannel + 5);
  return true;
}

static String meshLogicalZoneLabel(uint8_t logicalChannel) {
  uint8_t slot = 0;
  uint8_t relay = 0;
  if (!meshDecodeLogicalChannel(logicalChannel, slot, relay)) return String("Zone ") + logicalChannel;
  MeshGuard guard;
  if (!guard.acquired) return String("Zone ") + logicalChannel;
  const char* name = meshSlaves[slot].assigned && strlen(meshSlaves[slot].name)
      ? meshSlaves[slot].name
      : "Unassigned slave";
  return String("Zone ") + logicalChannel + " - " + name + " relay " + (relay + 1);
}

static void meshAddSchedulesToJson(JsonArray schedules) {
  MeshGuard guard;
  if (!guard.acquired) return;
  for (uint8_t i = 0; i < meshScheduleCount; i++) {
    MeshRemoteSchedule& schedule = meshSchedules[i];
    JsonObject item = schedules.createNestedObject();
    item["id"] = i;
    item["logicalChannel"] = schedule.logicalChannel;
    item["zone"] = meshLogicalZoneLabel(schedule.logicalChannel);
    item["enabled"] = schedule.enabled;
    item["startTime"] = htmlTimeValue(schedule.startHour, schedule.startMinute);
    item["durationSeconds"] = (uint32_t)schedule.runMinutes * 60UL;
    item["daysMask"] = schedule.daysMask;
    item["daysLabel"] = scheduleDaysMaskLabel(schedule.daysMask);
  }
}

static bool meshScheduleOverlaps(
    int startA,
    int endA,
    uint8_t daysA,
    int startB,
    int endB,
    uint8_t daysB) {
  return (daysA & daysB) != 0 && startA < endB && startB < endA;
}

static bool meshValidateAndReplaceSchedules(JsonArray schedules, String& errorOut) {
  if (schedules.size() > MESH_MAX_REMOTE_SCHEDULES) {
    errorOut = "too many slave-zone schedules";
    return false;
  }
  if (dailyScheduleCount + spigotScheduleCount + schedules.size() > MAX_DAILY_SCHEDULES) {
    errorOut = "combined local and slave schedule limit is 64";
    return false;
  }

  struct Interval {
    int start;
    int end;
    uint8_t days;
  } intervals[MESH_MAX_REMOTE_SCHEDULES + MAX_DAILY_SCHEDULES];
  uint8_t intervalCount = 0;

  for (uint8_t i = 0; i < dailyScheduleCount && intervalCount < sizeof(intervals) / sizeof(intervals[0]); i++) {
    if (!dailySchedules[i].enabled) continue;
    intervals[intervalCount++] = {
      dailySchedules[i].startHour * 60 + dailySchedules[i].startMinute,
      dailySchedules[i].startHour * 60 + dailySchedules[i].startMinute + dailySchedules[i].runMinutes,
      i < trackedScheduleCount ? scheduleDaysMasks[i] : (uint8_t)0x7F
    };
  }
  for (uint8_t i = 0; i < spigotScheduleCount && intervalCount < sizeof(intervals) / sizeof(intervals[0]); i++) {
    if (!spigotSchedules[i].enabled) continue;
    intervals[intervalCount++] = {
      spigotSchedules[i].startHour * 60 + spigotSchedules[i].startMinute,
      spigotSchedules[i].startHour * 60 + spigotSchedules[i].startMinute + spigotSchedules[i].runMinutes,
      spigotSchedules[i].daysMask
    };
  }

  MeshRemoteSchedule proposed[MESH_MAX_REMOTE_SCHEDULES];
  uint8_t proposedCount = 0;
  for (JsonObject item : schedules) {
    int channel = item["logicalChannel"] | (item["channel"] | 0);
    String start = String((const char*)(item["startTime"] | ""));
    int colon = start.indexOf(':');
    int hour = colon > 0 ? start.substring(0, colon).toInt() : -1;
    int minute = colon > 0 ? start.substring(colon + 1).toInt() : -1;
    int seconds = item["durationSeconds"] | 0;
    int runMinutes = (seconds + 59) / 60;
    bool enabled = item["enabled"].is<bool>() ? (bool)item["enabled"] : true;
    uint8_t daysMask = item["daysMask"].isNull() ? 0x7F : (uint8_t)(item["daysMask"].as<int>() & 0x7F);

    uint8_t slot = 0;
    uint8_t relay = 0;
    if (!meshDecodeLogicalChannel(channel, slot, relay) || !meshSlaves[slot].assigned) {
      errorOut = String("unknown slave zone ") + channel;
      return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || runMinutes < 1 || runMinutes > 240) {
      errorOut = "invalid slave-zone schedule row";
      return false;
    }

    int startMinutes = hour * 60 + minute;
    int endMinutes = startMinutes + runMinutes;
    if (enabled && (startMinutes < 4 * 60 || endMinutes > 20 * 60)) {
      errorOut = "enabled schedules must stay between 04:00 and 20:00";
      return false;
    }

    if (enabled) {
      for (uint8_t i = 0; i < intervalCount; i++) {
        if (meshScheduleOverlaps(startMinutes, endMinutes, daysMask, intervals[i].start, intervals[i].end, intervals[i].days)) {
          errorOut = "slave-zone schedule overlaps another enabled watering run on at least one selected day";
          return false;
        }
      }
      intervals[intervalCount++] = {startMinutes, endMinutes, daysMask};
    }

    MeshRemoteSchedule& out = proposed[proposedCount++];
    out.enabled = enabled;
    out.logicalChannel = channel;
    out.startHour = hour;
    out.startMinute = minute;
    out.runMinutes = runMinutes;
    out.daysMask = daysMask;
    out.lastRunYearDay = -1;
    out.lastRunMinuteOfDay = -1;
  }

  {
    MeshGuard guard;
    if (!guard.acquired) {
      errorOut = "mesh schedule storage busy";
      return false;
    }
    meshScheduleCount = proposedCount;
    for (uint8_t i = 0; i < proposedCount; i++) meshSchedules[i] = proposed[i];
  }
  meshSavePreferences();
  return true;
}

static void meshServiceMasterSchedules() {
  if (!meshIsMaster() || !clockIsValid()) return;
  struct tm now;
  if (!getLocalTime(&now, 20)) return;
  int minuteOfDay = now.tm_hour * 60 + now.tm_min;

  while (true) {
    int dueChannel = -1;
    uint32_t dueSeconds = 0;
    {
      MeshGuard guard;
      if (!guard.acquired) return;
      for (uint8_t i = 0; i < meshScheduleCount; i++) {
        MeshRemoteSchedule& schedule = meshSchedules[i];
        if (!schedule.enabled || schedule.runMinutes == 0 || (schedule.daysMask & (1U << now.tm_wday)) == 0) continue;
        int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
        if (scheduledMinute != minuteOfDay) continue;
        if (schedule.lastRunYearDay == now.tm_yday && schedule.lastRunMinuteOfDay == minuteOfDay) continue;

        schedule.lastRunYearDay = now.tm_yday;
        schedule.lastRunMinuteOfDay = minuteOfDay;
        dueChannel = schedule.logicalChannel;
        dueSeconds = (uint32_t)schedule.runMinutes * 60UL;
        break;
      }
    }

    if (dueChannel < 0) break;
    meshSendSlaveCommand((uint8_t)dueChannel, "on", dueSeconds);
  }
}

static void meshHandleMasterRelayControl() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in master mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  int channel = doc["logicalChannel"] | 0;
  String action = String((const char*)(doc["action"] | ""));
  uint32_t durationSeconds = doc["durationSeconds"] | DEFAULT_MANUAL_RUN_SECONDS;
  if (action != "on" && action != "off" && action != "toggle") {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid action\"}");
    return;
  }

  bool ok = meshSendSlaveCommand(channel, action, durationSeconds);
  server.send(ok ? 200 : 503, "application/json", ok
      ? "{\"ok\":true}"
      : "{\"ok\":false,\"error\":\"slave unavailable or command failed\"}");
}

static void meshHandleSchedulesGet() {
  DynamicJsonDocument doc(12288);
  doc["ok"] = true;
  JsonArray schedules = doc.createNestedArray("schedules");
  meshAddSchedulesToJson(schedules);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void meshHandleSchedulesPost() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"slave devices do not own schedules\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }
  DynamicJsonDocument doc(12288);
  if (deserializeJson(doc, server.arg("plain")) || !doc["schedules"].is<JsonArray>()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json or missing schedules\"}");
    return;
  }
  String error;
  if (!meshValidateAndReplaceSchedules(doc["schedules"].as<JsonArray>(), error)) {
    DynamicJsonDocument reply(384);
    reply["ok"] = false;
    reply["error"] = error;
    String body;
    serializeJson(reply, body);
    server.send(400, "application/json", body);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

static void meshAddStateJson(JsonDocument& doc) {
  doc["ok"] = true;
  doc["role"] = meshRoleName();
  doc["deviceId"] = meshDeviceId;
  doc["deviceName"] = meshDeviceName;
  doc["masterHost"] = meshMasterHost;
  doc["linkKey"] = meshLinkKey;
  doc["status"] = meshLastStatus;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["apSsid"] = apSsid;
  doc["apPass"] = apPass;
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["staSsid"] = staSsid;
  doc["staPass"] = staPass;
  doc["stationConnected"] = WiFi.status() == WL_CONNECTED;
  doc["stationIp"] = WiFi.localIP().toString();

  if (meshIsSlave()) {
    doc["masterOnline"] = meshLastMasterContactMs > 0 && millis() - meshLastMasterContactMs <= MESH_SLAVE_OFFLINE_MS;
    doc["assignedSlot"] = meshAssignedSlot;
    doc["baseLogicalChannel"] = meshAssignedBaseChannel;
    JsonArray relays = doc.createNestedArray("relays");
    for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
      JsonObject item = relays.createNestedObject();
      item["relay"] = relay + 1;
      item["on"] = relayState[relay];
      item["remainingSeconds"] = meshSlaveRemainingSeconds(relay);
    }
    return;
  }

  JsonArray slaves = doc.createNestedArray("slaves");
  {
    MeshGuard guard;
    if (guard.acquired) {
      for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
        MeshSlaveDevice& slave = meshSlaves[slot];
        if (!slave.assigned) continue;
        JsonObject item = slaves.createNestedObject();
        item["slot"] = slot;
        item["deviceId"] = slave.id;
        item["deviceName"] = slave.name;
        item["ip"] = slave.ip;
        item["online"] = meshSlaveOnline(slave);
        item["lastSeenMsAgo"] = slave.lastSeenMs ? millis() - slave.lastSeenMs : 0;
        item["baseLogicalChannel"] = meshLogicalChannel(slot, 0);
        JsonArray relays = item.createNestedArray("relays");
        for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
          JsonObject relayItem = relays.createNestedObject();
          relayItem["relay"] = relay + 1;
          relayItem["logicalChannel"] = meshLogicalChannel(slot, relay);
          relayItem["on"] = slave.relayOn[relay];
          relayItem["remainingSeconds"] = slave.remainingSeconds[relay];
        }
      }
    }
  }
  JsonArray schedules = doc.createNestedArray("schedules");
  meshAddSchedulesToJson(schedules);
}

static void meshHandleState() {
  DynamicJsonDocument doc(16384);
  meshAddStateJson(doc);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void meshWriteBaseWifiPreferences() {
  Preferences p;
  if (!p.begin("relay6", false)) return;
  p.putString("apSsid", apSsid);
  p.putString("apPass", apPass);
  p.putString("staSsid", staSsid);
  p.putString("staPass", staPass);
  p.end();
}

static void meshHandleConfigPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }
  DynamicJsonDocument doc(3072);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  String role = String((const char*)(doc["role"] | "master"));
  role.toLowerCase();
  meshRole = role == "slave" ? MESH_ROLE_SLAVE : MESH_ROLE_MASTER;
  if (doc["deviceId"].is<const char*>()) strlcpy(meshDeviceId, doc["deviceId"].as<const char*>(), sizeof(meshDeviceId));
  if (doc["deviceName"].is<const char*>()) strlcpy(meshDeviceName, doc["deviceName"].as<const char*>(), sizeof(meshDeviceName));
  if (doc["masterHost"].is<const char*>()) strlcpy(meshMasterHost, doc["masterHost"].as<const char*>(), sizeof(meshMasterHost));
  if (doc["linkKey"].is<const char*>()) strlcpy(meshLinkKey, doc["linkKey"].as<const char*>(), sizeof(meshLinkKey));
  if (doc["apSsid"].is<const char*>()) strlcpy(apSsid, doc["apSsid"].as<const char*>(), sizeof(apSsid));
  if (doc["staSsid"].is<const char*>()) strlcpy(staSsid, doc["staSsid"].as<const char*>(), sizeof(staSsid));
  if (doc["staPass"].is<const char*>()) strlcpy(staPass, doc["staPass"].as<const char*>(), sizeof(staPass));
  if (doc["apPass"].is<const char*>()) {
    String pass = doc["apPass"].as<String>();
    if (pass.length() == 0 || (pass.length() >= 8 && pass.length() <= 63)) strlcpy(apPass, pass.c_str(), sizeof(apPass));
  }
  meshGenerateDefaultIdentity();
  meshSavePreferences();
  meshWriteBaseWifiPreferences();

  allOff();
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(500);
  ESP.restart();
}

static void meshClearPreferencesNamespace(const char* name) {
  Preferences p;
  if (p.begin(name, false)) {
    p.clear();
    p.end();
  }
}

static void meshHandleFactoryReset() {
  meshStopAllSlaveRelays();
  meshClearPreferencesNamespace(MESH_PREF_NAMESPACE);
  meshClearPreferencesNamespace("relay6days");
  meshClearPreferencesNamespace("relay6spig");
  factoryReset();
  server.send(200, "text/plain", "Factory reset complete. Role returns to Master and Heroku synchronization returns to Off. Rebooting.");
  delay(500);
  ESP.restart();
}

static void meshHandleRoleAwareState() {
  if (meshIsMaster()) sendStateJson();
  else meshHandleState();
}

static void meshHandleRoleAwareRoot() {
  if (meshIsMaster()) {
    handleRoot();
    return;
  }

  String html = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Garden Relay Slave</title><style>body{font-family:Arial,sans-serif;background:#f1f5f9;color:#123;margin:0}.shell{max-width:800px;margin:auto;padding:14px}.card{background:#fff;border:1px solid #dbe7ef;border-radius:14px;padding:14px;margin:12px 0}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}button{padding:9px;border-radius:9px;border:1px solid #9cb8c8}.on{background:#dff8ea}@media(max-width:650px){.grid{grid-template-columns:1fr}}</style></head><body><div class="shell"><h1>Garden Relay Slave</h1><p><a href="/admin">Admin</a></p><div id="status" class="card">Loading...</div><div id="relays" class="grid"></div></div><script>
async function cmd(relay,action){const durationSeconds=Number(prompt('Run relay '+relay+' for how many seconds?','900')||0);if(action==='on'&&!durationSeconds)return;await fetch('/api/mesh/slave-local',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay,action,durationSeconds})});refresh()}
async function refresh(){const s=await (await fetch('/api/mesh/state',{cache:'no-store'})).json();status.textContent=s.deviceName+' · master '+(s.masterOnline?'online':'offline')+' · '+s.status;relays.innerHTML=(s.relays||[]).map(r=>`<div class="card ${r.on?'on':''}"><b>Slave relay ${r.relay}</b><p>${r.on?'ON · '+r.remainingSeconds+'s remaining':'OFF'}</p><button onclick="cmd(${r.relay},'on')">Run</button><button onclick="cmd(${r.relay},'off')">Stop</button></div>`).join('')}
refresh();setInterval(refresh,2000)</script></body></html>)HTML";
  server.send(200, "text/html", html);
}

static void meshHandleSlaveLocalControl() {
  if (!meshIsSlave() || !server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  DynamicJsonDocument doc(384);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  int relay = doc["relay"] | 0;
  String action = String((const char*)(doc["action"] | ""));
  uint32_t duration = doc["durationSeconds"] | DEFAULT_MANUAL_RUN_SECONDS;
  if (relay < 1 || relay > 6) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  if (action == "off") meshStopSlaveRelay(relay - 1);
  else meshStartSlaveRelay(relay - 1, duration);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void meshHandleRoleAwareManualRun() {
  if (meshIsMaster()) {
    handleManualRun();
    return;
  }
  int relay = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (relay < 1 || relay > 6 || minutes < 1 || minutes > 240) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"slave relays are 1-6\"}");
    return;
  }
  meshStartSlaveRelay(relay - 1, (uint32_t)minutes * 60UL);
  server.send(200, "application/json", "{\"ok\":true,\"mode\":\"slave-relay\"}");
}

static void meshHandleRoleAwareRelay() {
  if (meshIsMaster()) {
    handleRelay();
    return;
  }
  int relay = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int state = server.hasArg("state") ? server.arg("state").toInt() : 0;
  if (relay < 1 || relay > 6) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"slave relays are 1-6\"}");
    return;
  }
  if (state) meshStartSlaveRelay(relay - 1, DEFAULT_MANUAL_RUN_SECONDS);
  else meshStopSlaveRelay(relay - 1);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void meshHandleRoleAwareSpigots() {
  if (meshIsMaster()) handleSpigotRun();
  else server.send(409, "application/json", "{\"ok\":false,\"error\":\"relay 6 is a normal slave zone in slave mode\"}");
}

static void meshHandleRoleAwareAllOff() {
  if (meshIsMaster()) handleAllOff();
  else {
    meshStopAllSlaveRelays();
    server.send(200, "application/json", "{\"ok\":true}");
  }
}

static const char MESH_ADMIN_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Garden Controller Admin</title><style>
*{box-sizing:border-box}body{font-family:Inter,Arial,sans-serif;margin:0;background:#eef3f6;color:#123}.shell{max-width:1250px;margin:auto;padding:12px}.panel{background:#fff;border:1px solid #dbe7ef;border-radius:16px;padding:14px;margin-bottom:12px}.grid{display:grid;grid-template-columns:repeat(2,minmax(220px,1fr));gap:10px}.actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center}label{display:flex;flex-direction:column;gap:5px}input,select,button{padding:8px;border:1px solid #b9cbd7;border-radius:8px}button{cursor:pointer;background:#edf6ef}.danger{background:#9a2b2b;color:#fff}.slave-grid,.zone-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}.card{border:1px solid #d8e3eb;border-radius:12px;padding:10px}.online{background:#eaf9ef}.offline{background:#f6f6f6;color:#657}.schedule-row{border:1px solid #d8e3eb;border-radius:12px;padding:10px;margin:9px 0}.schedule-fields{display:grid;grid-template-columns:2fr 120px 100px 110px auto;gap:8px;align-items:end}.days{display:flex;gap:5px;flex-wrap:wrap;margin-top:8px}.days label{display:inline-flex;flex-direction:row;border:1px solid #c7d8e5;border-radius:999px;padding:5px 8px}iframe{width:100%;height:1700px;border:0;border-radius:14px;background:#fff}@media(max-width:760px){.grid,.schedule-fields{grid-template-columns:1fr}iframe{height:2200px}}
</style></head><body><div class="shell"><section class="panel"><h1>Garden Controller Master / Slave Administration</h1><p id="meshStatus">Loading...</p><p><a href="/">Controller View</a></p></section>
<section class="panel"><h2>Device Role and Link Settings</h2><p><b>Master</b> runs its local five zones and spigots and coordinates slave zones. <b>Slave</b> connects its Home WiFi to the master's access point and exposes all six physical relays as additional zones.</p><div class="grid"><label>Device role<select id="meshRole"><option value="master">Master</option><option value="slave">Slave</option></select></label><label>Device ID<input id="meshDeviceId"></label><label>Device name<input id="meshDeviceName"></label><label>Shared master/slave link key<input id="meshLinkKey"></label><label>Master address, slave mode<input id="meshMasterHost" placeholder="192.168.4.1"></label><label>Local AP SSID<input id="meshApSsid"></label><label>Local AP password<input id="meshApPass"></label><label>Home/Master WiFi SSID<input id="meshStaSsid"></label><label>Home/Master WiFi password<input id="meshStaPass"></label></div><p>In Slave mode, set Home/Master WiFi to the master's AP credentials. The slave keeps a recovery AP on 192.168.5.1 to avoid conflicting with the master's 192.168.4.1 AP.</p><div class="actions"><button onclick="saveMeshConfig()">Save Role Settings and Restart</button><button class="danger" onclick="factoryResetMesh()">Factory Reset</button></div></section>
<div id="masterPanels" hidden><section class="panel"><h2>Slave Presence and Added Zones</h2><div id="slaveList" class="slave-grid"></div><h3>Remote Zone Controls</h3><div id="remoteZones" class="zone-grid"></div></section><section class="panel"><h2>Slave Zone Schedules</h2><p>Slave schedules are owned and executed by the master. Logical channel 6 remains the master's spigots; the first slave uses Zones 7-12.</p><div id="meshScheduleRows"></div><div class="actions"><button onclick="addMeshSchedule()">Add Slave Schedule</button><button onclick="saveMeshSchedules()">Save Slave Schedules</button></div></section><section class="panel"><h2>Master Local Controller</h2><iframe src="/admin/core"></iframe></section></div>
<div id="slavePanels" hidden><section class="panel"><h2>Slave Relay Test Controls</h2><p>All six relays are independent slave zones. Automatic schedules run only on the master.</p><div id="slaveRelays" class="zone-grid"></div></section></div>
</div><script>
const DAY_NAMES=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];let meshState=null;let meshHydrated=false;
function e(id){return document.getElementById(id)}
function esc(v){return String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function rolePayload(){return{role:e('meshRole').value,deviceId:e('meshDeviceId').value,deviceName:e('meshDeviceName').value,linkKey:e('meshLinkKey').value,masterHost:e('meshMasterHost').value,apSsid:e('meshApSsid').value,apPass:e('meshApPass').value,staSsid:e('meshStaSsid').value,staPass:e('meshStaPass').value}}
async function saveMeshConfig(){if(!confirm('Save role settings and restart this controller?'))return;const r=await fetch('/api/mesh/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(rolePayload())});if(!r.ok){alert(await r.text());return}alert('Settings saved. The controller is restarting. Reconnect to its access point.')}
async function factoryResetMesh(){if(!confirm('Factory reset this controller?'))return;alert(await (await fetch('/api/factory-reset')).text())}
function zoneOptions(selected){const list=[];(meshState?.slaves||[]).forEach(s=>(s.relays||[]).forEach(r=>list.push([r.logicalChannel,`Zone ${r.logicalChannel} - ${s.deviceName} relay ${r.relay}`])));if(selected&&!list.some(x=>x[0]===Number(selected)))list.push([Number(selected),`Zone ${selected} - unavailable slave`]);return list.map(([v,l])=>`<option value="${v}"${Number(selected)===v?' selected':''}>${esc(l)}</option>`).join('')}
function dayMask(row){return [...row.querySelectorAll('.day')].reduce((m,x)=>x.checked?m|Number(x.value):m,0)}
function addMeshSchedule(v={}){const row=document.createElement('div');row.className='schedule-row';const channel=Number(v.logicalChannel||v.channel||((meshState?.slaves?.[0]?.baseLogicalChannel)||7));const mask=Number(v.daysMask??127);row.innerHTML=`<div class="schedule-fields"><label>Slave zone<select class="channel">${zoneOptions(channel)}</select></label><label>Start<input class="start" type="time" value="${esc(v.startTime||'06:00')}"></label><label>Minutes<input class="minutes" type="number" min="1" max="240" value="${Math.max(1,Math.round(Number(v.durationSeconds||600)/60))}"></label><label>Enabled<select class="enabled"><option value="on"${v.enabled===false?'':' selected'}>On</option><option value="off"${v.enabled===false?' selected':''}>Off</option></select></label><button class="danger" onclick="this.closest('.schedule-row').remove()">Delete</button></div><div class="days">${DAY_NAMES.map((d,i)=>`<label><input class="day" type="checkbox" value="${1<<i}"${mask&(1<<i)?' checked':''}>${d}</label>`).join('')}</div>`;e('meshScheduleRows').appendChild(row)}
async function saveMeshSchedules(){const schedules=[...document.querySelectorAll('#meshScheduleRows .schedule-row')].map(r=>({logicalChannel:Number(r.querySelector('.channel').value),startTime:r.querySelector('.start').value,durationSeconds:Number(r.querySelector('.minutes').value)*60,enabled:r.querySelector('.enabled').value==='on',daysMask:dayMask(r)}));const res=await fetch('/api/mesh/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({schedules})});const body=await res.json().catch(()=>({error:'save failed'}));if(!res.ok){alert(body.error||'Save failed');return}alert('Slave schedules saved');await refreshMesh(true)}
async function remoteCmd(channel,action){let durationSeconds=0;if(action==='on'){const m=Number(prompt('Run Zone '+channel+' for how many minutes?','15')||0);if(!m)return;durationSeconds=m*60}const r=await fetch('/api/mesh/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({logicalChannel:channel,action,durationSeconds})});if(!r.ok)alert((await r.json().catch(()=>({error:'command failed'}))).error||'Command failed');refreshMesh(false)}
async function slaveCmd(relay,action){let durationSeconds=0;if(action==='on'){const m=Number(prompt('Run slave relay '+relay+' for how many minutes?','15')||0);if(!m)return;durationSeconds=m*60}await fetch('/api/mesh/slave-local',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay,action,durationSeconds})});refreshMesh(false)}
function renderMaster(s,force){e('masterPanels').hidden=false;e('slavePanels').hidden=true;e('slaveList').innerHTML=(s.slaves||[]).length?(s.slaves||[]).map(x=>`<div class="card ${x.online?'online':'offline'}"><b>${esc(x.deviceName)}</b><p>${x.online?'Online at '+esc(x.ip):'Offline'} · Zones ${x.baseLogicalChannel}-${x.baseLogicalChannel+5}</p><small>${esc(x.deviceId)}</small></div>`).join(''):'<p>No slaves have reported yet.</p>';e('remoteZones').innerHTML=(s.slaves||[]).flatMap(x=>(x.relays||[]).map(r=>`<div class="card ${x.online?'online':'offline'}"><b>Zone ${r.logicalChannel}</b><p>${esc(x.deviceName)} relay ${r.relay} · ${r.on?'ON':'OFF'}${r.remainingSeconds?' · '+r.remainingSeconds+'s':''}</p><button ${x.online?'':'disabled'} onclick="remoteCmd(${r.logicalChannel},'on')">Run</button><button ${x.online?'':'disabled'} onclick="remoteCmd(${r.logicalChannel},'off')">Stop</button></div>`)).join('');if(force){e('meshScheduleRows').innerHTML='';(s.schedules||[]).forEach(addMeshSchedule)}}
function renderSlave(s){e('masterPanels').hidden=true;e('slavePanels').hidden=false;e('slaveRelays').innerHTML=(s.relays||[]).map(r=>`<div class="card ${r.on?'online':''}"><b>Slave relay ${r.relay}</b><p>${r.on?'ON · '+r.remainingSeconds+'s remaining':'OFF'}</p><button onclick="slaveCmd(${r.relay},'on')">Run</button><button onclick="slaveCmd(${r.relay},'off')">Stop</button></div>`).join('')}
async function refreshMesh(force=false){const s=await (await fetch('/api/mesh/state',{cache:'no-store'})).json();meshState=s;e('meshStatus').textContent=`${s.deviceName} · ${s.role.toUpperCase()} · ${s.status} · AP ${s.apSsid} ${s.apIp}`;if(!meshHydrated||force&&document.activeElement===document.body){e('meshRole').value=s.role;e('meshDeviceId').value=s.deviceId||'';e('meshDeviceName').value=s.deviceName||'';e('meshLinkKey').value=s.linkKey||'';e('meshMasterHost').value=s.masterHost||'';e('meshApSsid').value=s.apSsid||'';e('meshApPass').value=s.apPass||'';e('meshStaSsid').value=s.staSsid||'';e('meshStaPass').value=s.staPass||'';meshHydrated=true}if(s.role==='master')renderMaster(s,force||!e('meshScheduleRows').children.length);else renderSlave(s)}
refreshMesh(true);setInterval(()=>refreshMesh(false),2000)
</script></body></html>
)HTML";

static void meshHandleAdminShell() {
  server.send_P(200, "text/html", MESH_ADMIN_PAGE);
}

static void meshHandleAdminCore() {
  if (!meshIsMaster()) {
    server.send(409, "text/plain", "The legacy local controller panel is disabled in Slave mode.");
    return;
  }
  server.send(200, "text/html", buildIntegratedAdminPage());
}

static void meshTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (meshIsSlave()) {
      // A slave never talks directly to Heroku. It reports only to its master.
      remoteEnabled = false;
      if (millis() > 1000UL) meshNeutralizeLegacyControllerForSlave();
      meshServiceSlaveTimers();
      if (WiFi.status() == WL_CONNECTED && millis() - meshLastHeartbeatMs >= MESH_HEARTBEAT_INTERVAL_MS) {
        meshLastHeartbeatMs = millis();
        meshSendHeartbeat();
      }
    } else {
      meshServiceMasterSchedules();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void meshEarlyInit() {
  meshMutex = xSemaphoreCreateRecursiveMutex();
  meshLoadPreferences();
  if (meshIsSlave()) {
    meshSetSlaveApSubnet();
    remoteEnabled = false;
  }

  // Registered before setupServer(); WebServer evaluates handlers in insertion
  // order, allowing one firmware image to provide role-aware replacements while
  // retaining all legacy routes and behavior for Master mode.
  server.on("/", HTTP_GET, meshHandleRoleAwareRoot);
  server.on("/admin", HTTP_GET, meshHandleAdminShell);
  server.on("/admin/core", HTTP_GET, meshHandleAdminCore);
  server.on("/api/state", HTTP_GET, meshHandleRoleAwareState);
  server.on("/status", HTTP_GET, meshHandleRoleAwareState);
  server.on("/api/manual-run", HTTP_GET, meshHandleRoleAwareManualRun);
  server.on("/api/relay", HTTP_GET, meshHandleRoleAwareRelay);
  server.on("/api/spigots-run", HTTP_GET, meshHandleRoleAwareSpigots);
  server.on("/api/alloff", HTTP_GET, meshHandleRoleAwareAllOff);
  server.on("/api/factory-reset", HTTP_GET, meshHandleFactoryReset);

  server.on("/api/mesh/state", HTTP_GET, meshHandleState);
  server.on("/api/mesh/config", HTTP_POST, meshHandleConfigPost);
  server.on("/api/mesh/relay", HTTP_POST, meshHandleMasterRelayControl);
  server.on("/api/mesh/schedules", HTTP_GET, meshHandleSchedulesGet);
  server.on("/api/mesh/schedules", HTTP_POST, meshHandleSchedulesPost);
  server.on("/api/mesh/slave-local", HTTP_POST, meshHandleSlaveLocalControl);
  server.on("/api/slaves/register", HTTP_POST, meshHandleSlaveHeartbeat);
  server.on("/api/slaves/heartbeat", HTTP_POST, meshHandleSlaveHeartbeat);
  server.on("/api/slave/relay", HTTP_POST, meshHandleSlaveRelayCommand);
  server.on("/api/slave/alloff", HTTP_POST, meshHandleSlaveAllOff);

  xTaskCreatePinnedToCore(
      meshTask,
      "meshTask",
      12288,
      nullptr,
      1,
      &meshTaskHandle,
      0);
}
