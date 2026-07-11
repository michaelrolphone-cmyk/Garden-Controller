#include <WiFiUdp.h>

// Slave Internet time fallback.
//
// A slave normally joins the master's AP and receives authoritative time in the
// heartbeat response. If the master cannot be reached, the same station radio
// temporarily joins a separately configured Internet-capable WiFi network,
// obtains a directly verified NTP response, sets the system clock, and returns
// to the master AP. Local schedules continue to be executed by the reliable
// slave scheduler throughout ordinary network interruptions.

static const char SLAVE_TIME_PREF_NAMESPACE[] = "r6timefallback";
static const uint32_t SLAVE_TIME_MASTER_GRACE_MS = 45000UL;
static const uint32_t SLAVE_TIME_CONNECT_TIMEOUT_MS = 30000UL;
static const uint32_t SLAVE_TIME_NTP_TIMEOUT_MS = 2500UL;
static const uint32_t SLAVE_TIME_STATUS_REPORT_MS = 30000UL;
static const uint16_t SLAVE_TIME_NTP_LOCAL_PORT = 2390;

static char slaveTimeFallbackSsid[33] = "";
static char slaveTimeFallbackPass[65] = "";
static char slaveTimeNtpServer1[64] = "pool.ntp.org";
static char slaveTimeNtpServer2[64] = "time.nist.gov";
static uint16_t slaveTimeResyncHours = 6;
static uint16_t slaveTimeMasterRetryMinutes = 5;
static uint32_t slaveTimeLastInternetSyncEpoch = 0;
static uint32_t slaveTimeLastInternetAttemptMs = 0;
static uint32_t slaveTimeMasterLostSinceMs = 0;
static uint32_t slaveTimePhaseStartedMs = 0;
static uint32_t slaveTimeLastStatusReportMs = 0;
static uint32_t slaveTimeNextMasterRetryMs = 0;
static bool slaveTimeNtpAttempted = false;
static String slaveTimeSource = "unsynchronized";
static String slaveTimeStatus = "time fallback starting";
static TaskHandle_t slaveTimeTaskHandle = nullptr;

enum SlaveTimePhase : uint8_t {
  SLAVE_TIME_WAIT_MASTER = 0,
  SLAVE_TIME_CONNECTING_INTERNET = 1,
  SLAVE_TIME_SYNCING_NTP = 2,
  SLAVE_TIME_RETURNING_MASTER = 3
};

static SlaveTimePhase slaveTimePhase = SLAVE_TIME_WAIT_MASTER;

struct SlaveClockObservation {
  bool reported;
  bool clockValid;
  uint32_t lastInternetSyncEpoch;
  uint32_t lastReportMs;
  char source[20];
  char status[96];
};

static SlaveClockObservation slaveClockObservations[MESH_MAX_SLAVES];

static void slaveTimeSavePreferences() {
  Preferences p;
  if (!p.begin(SLAVE_TIME_PREF_NAMESPACE, false)) return;
  p.putString("ssid", slaveTimeFallbackSsid);
  p.putString("pass", slaveTimeFallbackPass);
  p.putString("ntp1", slaveTimeNtpServer1);
  p.putString("ntp2", slaveTimeNtpServer2);
  p.putUShort("resyncH", slaveTimeResyncHours);
  p.putUShort("retryM", slaveTimeMasterRetryMinutes);
  p.putUInt("lastNtp", slaveTimeLastInternetSyncEpoch);
  p.end();
}

static void slaveTimeLoadPreferences() {
  Preferences p;
  if (!p.begin(SLAVE_TIME_PREF_NAMESPACE, true)) return;
  String value = p.getString("ssid", "");
  strlcpy(slaveTimeFallbackSsid, value.c_str(), sizeof(slaveTimeFallbackSsid));
  value = p.getString("pass", "");
  strlcpy(slaveTimeFallbackPass, value.c_str(), sizeof(slaveTimeFallbackPass));
  value = p.getString("ntp1", "pool.ntp.org");
  strlcpy(slaveTimeNtpServer1, value.c_str(), sizeof(slaveTimeNtpServer1));
  value = p.getString("ntp2", "time.nist.gov");
  strlcpy(slaveTimeNtpServer2, value.c_str(), sizeof(slaveTimeNtpServer2));
  slaveTimeResyncHours = constrain((int)p.getUShort("resyncH", 6), 1, 168);
  slaveTimeMasterRetryMinutes = constrain((int)p.getUShort("retryM", 5), 1, 60);
  slaveTimeLastInternetSyncEpoch = p.getUInt("lastNtp", 0);
  p.end();
}

static bool slaveTimeFallbackConfigured() {
  return strlen(slaveTimeFallbackSsid) > 0;
}

static bool slaveTimeConnectedTo(const char* ssid) {
  if (WiFi.status() != WL_CONNECTED || !ssid || !strlen(ssid)) return false;
  return WiFi.SSID() == String(ssid);
}

static bool slaveTimeMasterRecentlySeen() {
  return meshLastMasterContactMs > 0 &&
      millis() - meshLastMasterContactMs <= MESH_SLAVE_OFFLINE_MS;
}

static uint32_t slaveTimeResyncIntervalMs() {
  return (uint32_t)slaveTimeResyncHours * 60UL * 60UL * 1000UL;
}

static uint32_t slaveTimeMasterRetryMs() {
  return (uint32_t)slaveTimeMasterRetryMinutes * 60UL * 1000UL;
}

static bool slaveTimeNeedsInternetSync() {
  if (!clockIsValid()) return true;
  if (slaveTimeMasterLostSinceMs == 0) return false;
  return millis() - slaveTimeMasterLostSinceMs >= slaveTimeResyncIntervalMs();
}

static void slaveTimeSetClock(uint32_t epoch) {
  if (epoch <= 1700000000UL) return;
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  slaveTimeLastInternetSyncEpoch = epoch;
  slaveTimeSource = "internet-ntp";
  slaveTimeStatus = "clock synchronized from Internet NTP";
  slaveTimeSavePreferences();
}

static bool slaveTimeQueryNtp(const char* hostname, uint32_t& epochOut) {
  epochOut = 0;
  if (!hostname || !strlen(hostname) || WiFi.status() != WL_CONNECTED) return false;

  IPAddress address;
  if (WiFi.hostByName(hostname, address) != 1) return false;

  WiFiUDP udp;
  if (!udp.begin(SLAVE_TIME_NTP_LOCAL_PORT)) return false;

  uint8_t packet[48];
  memset(packet, 0, sizeof(packet));
  packet[0] = 0b11100011;
  packet[1] = 0;
  packet[2] = 6;
  packet[3] = 0xEC;
  packet[12] = 49;
  packet[13] = 0x4E;
  packet[14] = 49;
  packet[15] = 52;

  if (!udp.beginPacket(address, 123)) {
    udp.stop();
    return false;
  }
  udp.write(packet, sizeof(packet));
  if (!udp.endPacket()) {
    udp.stop();
    return false;
  }

  uint32_t started = millis();
  while (millis() - started < SLAVE_TIME_NTP_TIMEOUT_MS) {
    int size = udp.parsePacket();
    if (size >= 48) {
      int read = udp.read(packet, sizeof(packet));
      udp.stop();
      if (read < 48) return false;

      uint32_t seconds1900 =
          ((uint32_t)packet[40] << 24) |
          ((uint32_t)packet[41] << 16) |
          ((uint32_t)packet[42] << 8) |
          (uint32_t)packet[43];
      if (seconds1900 <= 2208988800UL) return false;
      uint32_t epoch = seconds1900 - 2208988800UL;
      if (epoch <= 1700000000UL) return false;
      epochOut = epoch;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  udp.stop();
  return false;
}

static void slaveTimeConnectInternet() {
  if (!slaveTimeFallbackConfigured()) return;
  WiFi.disconnect(false, false);
  delay(20);
  WiFi.begin(slaveTimeFallbackSsid, slaveTimeFallbackPass);
  staConnectInProgress = false;
  lastWifiAttemptMs = millis();
  slaveTimePhase = SLAVE_TIME_CONNECTING_INTERNET;
  slaveTimePhaseStartedMs = millis();
  slaveTimeLastInternetAttemptMs = millis();
  slaveTimeNtpAttempted = false;
  slaveTimeStatus = String("connecting to Internet time WiFi ") + slaveTimeFallbackSsid;
}

static void slaveTimeConnectMaster() {
  WiFi.disconnect(false, false);
  delay(20);
  slaveTimePhase = SLAVE_TIME_RETURNING_MASTER;
  slaveTimePhaseStartedMs = millis();
  slaveTimeNextMasterRetryMs = millis() + slaveTimeMasterRetryMs();
  slaveTimeStatus = "returning to master WiFi";
  if (strlen(staSsid) > 0) {
    connectSta(false);
  }
}

static String slaveTimeNetworkRole() {
  if (WiFi.status() != WL_CONNECTED) return "disconnected";
  if (slaveTimeConnectedTo(staSsid)) return "master-ap";
  if (slaveTimeConnectedTo(slaveTimeFallbackSsid)) return "internet-time";
  return "other";
}

static void slaveTimeClearClockObservations() {
  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    slaveClockObservations[slot].reported = false;
    slaveClockObservations[slot].clockValid = false;
    slaveClockObservations[slot].lastInternetSyncEpoch = 0;
    slaveClockObservations[slot].lastReportMs = 0;
    slaveClockObservations[slot].source[0] = 0;
    slaveClockObservations[slot].status[0] = 0;
  }
}

static bool slaveTimeReportStatusToMaster() {
  if (!meshIsSlave() || !slaveTimeConnectedTo(staSsid)) return false;

  DynamicJsonDocument doc(1024);
  doc["linkKey"] = meshLinkKey;
  doc["deviceId"] = meshDeviceId;
  doc["deviceName"] = meshDeviceName;
  doc["clockValid"] = clockIsValid();
  doc["timeSource"] = slaveTimeSource;
  doc["timeStatus"] = slaveTimeStatus;
  doc["lastInternetSyncEpoch"] = slaveTimeLastInternetSyncEpoch;
  doc["fallbackConfigured"] = slaveTimeFallbackConfigured();

  String body;
  serializeJson(doc, body);
  String response;
  int code = -1;
  return meshPostJson(
      meshMasterBaseUrl() + "/api/slaves/time-status",
      body,
      response,
      code);
}

static void slaveTimeHandleStatusReport() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in master mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(1536);
  if (deserializeJson(doc, server.arg("plain")) || !meshVerifyLinkKey(doc)) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid slave time report\"}");
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

  SlaveClockObservation& observation = slaveClockObservations[slot];
  observation.reported = true;
  observation.clockValid = doc["clockValid"] | false;
  observation.lastInternetSyncEpoch = doc["lastInternetSyncEpoch"] | 0;
  observation.lastReportMs = millis();
  String value = String((const char*)(doc["timeSource"] | "unknown"));
  strlcpy(observation.source, value.c_str(), sizeof(observation.source));
  value = String((const char*)(doc["timeStatus"] | ""));
  strlcpy(observation.status, value.c_str(), sizeof(observation.status));
  server.send(200, "application/json", "{\"ok\":true}");
}

static void slaveTimeAddState(JsonDocument& doc) {
  JsonObject fallback = doc.createNestedObject("internetTimeFallback");
  fallback["configured"] = slaveTimeFallbackConfigured();
  fallback["ssid"] = slaveTimeFallbackSsid;
  fallback["password"] = slaveTimeFallbackPass;
  fallback["ntpServer1"] = slaveTimeNtpServer1;
  fallback["ntpServer2"] = slaveTimeNtpServer2;
  fallback["resyncHours"] = slaveTimeResyncHours;
  fallback["masterRetryMinutes"] = slaveTimeMasterRetryMinutes;
  fallback["clockValid"] = clockIsValid();
  fallback["timeSource"] = slaveTimeSource;
  fallback["status"] = slaveTimeStatus;
  fallback["lastInternetSyncEpoch"] = slaveTimeLastInternetSyncEpoch;
  fallback["networkRole"] = slaveTimeNetworkRole();
  fallback["connectedSsid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";

  if (!meshIsMaster()) return;

  JsonArray slaves = doc["slaves"].as<JsonArray>();
  for (JsonObject slave : slaves) {
    int slot = slave["slot"] | -1;
    if (slot < 0 || slot >= MESH_MAX_SLAVES) continue;
    SlaveClockObservation& observation = slaveClockObservations[slot];
    JsonObject clock = slave.createNestedObject("clock");
    clock["reported"] = observation.reported;
    clock["valid"] = observation.clockValid;
    clock["source"] = observation.source;
    clock["status"] = observation.status;
    clock["lastInternetSyncEpoch"] = observation.lastInternetSyncEpoch;
    clock["reportedMsAgo"] = observation.lastReportMs
        ? millis() - observation.lastReportMs
        : 0;
  }
}

static void slaveTimeHandleState() {
  DynamicJsonDocument doc(28672);
  reliableAddState(doc);
  slaveTimeAddState(doc);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void slaveTimeHandleConfigPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  String fallbackSsid = doc["timeFallbackSsid"].is<const char*>()
      ? doc["timeFallbackSsid"].as<String>()
      : String(slaveTimeFallbackSsid);
  String fallbackPass = doc["timeFallbackPass"].is<const char*>()
      ? doc["timeFallbackPass"].as<String>()
      : String(slaveTimeFallbackPass);
  String ntp1 = doc["timeNtpServer1"].is<const char*>()
      ? doc["timeNtpServer1"].as<String>()
      : String(slaveTimeNtpServer1);
  String ntp2 = doc["timeNtpServer2"].is<const char*>()
      ? doc["timeNtpServer2"].as<String>()
      : String(slaveTimeNtpServer2);
  int resyncHours = doc["timeResyncHours"] | slaveTimeResyncHours;
  int retryMinutes = doc["timeMasterRetryMinutes"] | slaveTimeMasterRetryMinutes;
  String role = String((const char*)(doc["role"] | meshRoleName().c_str()));
  String masterSsid = doc["staSsid"].is<const char*>()
      ? doc["staSsid"].as<String>()
      : String(staSsid);

  fallbackSsid.trim();
  ntp1.trim();
  ntp2.trim();
  role.toLowerCase();

  if (fallbackSsid.length() > 32 || fallbackPass.length() > 64 ||
      (fallbackPass.length() > 0 && fallbackPass.length() < 8)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"fallback WiFi credentials are invalid\"}");
    return;
  }
  if (ntp1.length() == 0 || ntp1.length() > 63 || ntp2.length() > 63) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"NTP server names are invalid\"}");
    return;
  }
  if (resyncHours < 1 || resyncHours > 168 || retryMinutes < 1 || retryMinutes > 60) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"time fallback intervals are invalid\"}");
    return;
  }
  if (role == "slave" && fallbackSsid.length() && fallbackSsid == masterSsid) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"fallback Internet WiFi must be different from the master AP\"}");
    return;
  }

  strlcpy(slaveTimeFallbackSsid, fallbackSsid.c_str(), sizeof(slaveTimeFallbackSsid));
  strlcpy(slaveTimeFallbackPass, fallbackPass.c_str(), sizeof(slaveTimeFallbackPass));
  strlcpy(slaveTimeNtpServer1, ntp1.c_str(), sizeof(slaveTimeNtpServer1));
  strlcpy(slaveTimeNtpServer2, ntp2.c_str(), sizeof(slaveTimeNtpServer2));
  slaveTimeResyncHours = resyncHours;
  slaveTimeMasterRetryMinutes = retryMinutes;
  slaveTimeSavePreferences();

  // Delegate the remaining role, mesh, AP, and station settings to the existing
  // transactional handler. It persists them, turns relays off, and restarts.
  meshHandleConfigPost();
}

static String slaveTimeBuildAdminPage() {
  String page = reliableBuildAdminPage();
  page.reserve(page.length() + 7000);

  const String wifiAnchor =
      "<label>Home/Master WiFi password<input id=\"meshStaPass\"></label></div><p>";
  const String fallbackFields =
      "<label>Fallback Internet WiFi SSID<input id=\"timeFallbackSsid\"></label>"
      "<label>Fallback Internet WiFi password<input id=\"timeFallbackPass\" type=\"password\"></label>"
      "<label>Primary NTP server<input id=\"timeNtpServer1\" value=\"pool.ntp.org\"></label>"
      "<label>Secondary NTP server<input id=\"timeNtpServer2\" value=\"time.nist.gov\"></label>"
      "<label>Internet resync interval, hours<input id=\"timeResyncHours\" type=\"number\" min=\"1\" max=\"168\" value=\"6\"></label>"
      "<label>Master retry interval, minutes<input id=\"timeMasterRetryMinutes\" type=\"number\" min=\"1\" max=\"60\" value=\"5\"></label>"
      "</div><p id=\"timeFallbackStatus\"></p><p>";
  if (page.indexOf(wifiAnchor) >= 0) {
    page.replace(wifiAnchor,
        "<label>Home/Master WiFi password<input id=\"meshStaPass\"></label>" +
        fallbackFields);
  }

  const String scriptMarker = "</script></body></html>";
  const String script = R"JS(
const slaveTimeBaseRolePayload=rolePayload;
rolePayload=function(){
  return Object.assign(slaveTimeBaseRolePayload(),{
    timeFallbackSsid:e('timeFallbackSsid').value,
    timeFallbackPass:e('timeFallbackPass').value,
    timeNtpServer1:e('timeNtpServer1').value,
    timeNtpServer2:e('timeNtpServer2').value,
    timeResyncHours:Number(e('timeResyncHours').value),
    timeMasterRetryMinutes:Number(e('timeMasterRetryMinutes').value)
  });
};
let slaveTimeFieldsHydrated=false;
const slaveTimeBaseRenderMaster=renderMaster;
renderMaster=function(s,force){
  slaveTimeBaseRenderMaster(s,force);
  const cards=[...e('slaveList').children];
  (s.slaves||[]).forEach((slave,index)=>{
    const clock=slave.clock||{};
    const p=document.createElement('p');
    p.textContent=clock.reported
      ? `Clock ${clock.valid?'valid':'INVALID'} via ${clock.source||'unknown'}${clock.lastInternetSyncEpoch?' · Internet sync '+fmtEpoch(clock.lastInternetSyncEpoch):''}`
      : 'Clock status not yet reported';
    if(!clock.valid)p.style.fontWeight='bold';
    if(cards[index])cards[index].appendChild(p);
  });
};
const slaveTimeBaseRefresh=refreshMesh;
refreshMesh=async function(force=false){
  await slaveTimeBaseRefresh(force);
  const tf=meshState?.internetTimeFallback||{};
  if(!slaveTimeFieldsHydrated){
    e('timeFallbackSsid').value=tf.ssid||'';
    e('timeFallbackPass').value=tf.password||'';
    e('timeNtpServer1').value=tf.ntpServer1||'pool.ntp.org';
    e('timeNtpServer2').value=tf.ntpServer2||'time.nist.gov';
    e('timeResyncHours').value=tf.resyncHours||6;
    e('timeMasterRetryMinutes').value=tf.masterRetryMinutes||5;
    slaveTimeFieldsHydrated=true;
  }
  const status=e('timeFallbackStatus');
  if(status){
    status.textContent=`Clock ${tf.clockValid?'valid':'INVALID'} · source ${tf.timeSource||'unknown'} · network ${tf.networkRole||'unknown'} · ${tf.status||''}${tf.lastInternetSyncEpoch?' · last Internet sync '+fmtEpoch(tf.lastInternetSyncEpoch):''}`;
    status.style.fontWeight=tf.clockValid?'normal':'bold';
  }
};
refreshMesh(true);
)JS";
  if (page.indexOf(scriptMarker) >= 0) {
    page.replace(scriptMarker, script + scriptMarker);
  }
  return page;
}

static void slaveTimeHandleAdmin() {
  server.send(200, "text/html", slaveTimeBuildAdminPage());
}

static void slaveTimeHandleFactoryReset() {
  meshClearPreferencesNamespace(SLAVE_TIME_PREF_NAMESPACE);
  reliableHandleFactoryReset();
}

static void slaveTimeService() {
  if (!meshIsSlave()) return;

  bool masterRecent = slaveTimeMasterRecentlySeen();
  bool onMaster = slaveTimeConnectedTo(staSsid);
  bool onInternet = slaveTimeConnectedTo(slaveTimeFallbackSsid);

  if (slaveTimePhase != SLAVE_TIME_WAIT_MASTER) {
    // Prevent the reliable heartbeat from repeatedly timing out against the
    // master's private address while the station radio is on Internet WiFi.
    reliableLastHeartbeatMs = millis();
  }

  if (masterRecent && onMaster) {
    slaveTimeMasterLostSinceMs = 0;
    slaveTimePhase = SLAVE_TIME_WAIT_MASTER;
    slaveTimeSource = clockIsValid() ? "master" : "unsynchronized";
    slaveTimeStatus = clockIsValid()
        ? "clock confirmed from master heartbeat"
        : "master connected but clock is not valid";
    return;
  }

  if (slaveTimeMasterLostSinceMs == 0) {
    slaveTimeMasterLostSinceMs = millis();
  }

  if (!slaveTimeFallbackConfigured()) {
    if (!clockIsValid()) {
      slaveTimeSource = "unsynchronized";
      slaveTimeStatus = "CRITICAL: clock invalid and Internet time WiFi is not configured";
    } else {
      slaveTimeStatus = "master unavailable; continuing with retained clock";
    }
    return;
  }

  switch (slaveTimePhase) {
    case SLAVE_TIME_WAIT_MASTER: {
      bool graceExpired = millis() - slaveTimeMasterLostSinceMs >= SLAVE_TIME_MASTER_GRACE_MS;
      bool retryDue = slaveTimeLastInternetAttemptMs == 0 ||
          millis() - slaveTimeLastInternetAttemptMs >= slaveTimeMasterRetryMs();
      if (graceExpired && retryDue && slaveTimeNeedsInternetSync()) {
        slaveTimeConnectInternet();
      } else if (!clockIsValid()) {
        slaveTimeSource = "unsynchronized";
        slaveTimeStatus = graceExpired
            ? "clock invalid; waiting to retry Internet time WiFi"
            : "clock invalid; waiting briefly for master before Internet fallback";
      } else {
        slaveTimeStatus = "master unavailable; local schedules using retained clock";
      }
      break;
    }

    case SLAVE_TIME_CONNECTING_INTERNET:
      if (onInternet) {
        slaveTimePhase = SLAVE_TIME_SYNCING_NTP;
        slaveTimePhaseStartedMs = millis();
        slaveTimeNtpAttempted = false;
        slaveTimeStatus = "Internet WiFi connected; requesting NTP time";
      } else if (millis() - slaveTimePhaseStartedMs >= SLAVE_TIME_CONNECT_TIMEOUT_MS) {
        slaveTimeStatus = "Internet time WiFi connection failed; returning to master WiFi";
        slaveTimeConnectMaster();
      }
      break;

    case SLAVE_TIME_SYNCING_NTP:
      if (!onInternet) {
        slaveTimeStatus = "Internet WiFi dropped before NTP completed";
        slaveTimeConnectMaster();
        break;
      }
      if (!slaveTimeNtpAttempted) {
        slaveTimeNtpAttempted = true;
        uint32_t epoch = 0;
        bool synchronized = slaveTimeQueryNtp(slaveTimeNtpServer1, epoch);
        if (!synchronized && strlen(slaveTimeNtpServer2)) {
          synchronized = slaveTimeQueryNtp(slaveTimeNtpServer2, epoch);
        }
        if (synchronized) {
          slaveTimeSetClock(epoch);
        } else {
          slaveTimeStatus = "Internet connected but both NTP servers failed";
        }
        slaveTimeConnectMaster();
      }
      break;

    case SLAVE_TIME_RETURNING_MASTER:
      if (onMaster) {
        slaveTimePhase = SLAVE_TIME_WAIT_MASTER;
        slaveTimeStatus = "master WiFi connected; awaiting heartbeat confirmation";
      } else if (millis() - slaveTimePhaseStartedMs >= SLAVE_TIME_CONNECT_TIMEOUT_MS) {
        slaveTimePhase = SLAVE_TIME_WAIT_MASTER;
        slaveTimeStatus = clockIsValid()
            ? "master still unavailable; schedules continue with synchronized clock"
            : "CRITICAL: master and Internet time are unavailable; clock invalid";
      }
      break;
  }
}

static void slaveTimeTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (meshIsSlave()) {
      slaveTimeService();
      if (slaveTimeConnectedTo(staSsid) &&
          millis() - slaveTimeLastStatusReportMs >= SLAVE_TIME_STATUS_REPORT_MS) {
        slaveTimeLastStatusReportMs = millis();
        slaveTimeReportStatusToMaster();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void slaveInternetTimePreInit() {
  // Register before the reliable and compatibility layers so these extended
  // handlers remain authoritative for the shared routes.
  server.on("/admin", HTTP_GET, slaveTimeHandleAdmin);
  server.on("/api/mesh/config", HTTP_POST, slaveTimeHandleConfigPost);
  server.on("/api/mesh/state", HTTP_GET, slaveTimeHandleState);
  server.on("/api/slaves/time-status", HTTP_POST, slaveTimeHandleStatusReport);
  server.on("/api/factory-reset", HTTP_GET, slaveTimeHandleFactoryReset);
}

void slaveInternetTimePostInit() {
  slaveTimeLoadPreferences();
  slaveTimeClearClockObservations();
  if (clockIsValid()) {
    slaveTimeSource = meshIsSlave() ? "retained" : "local";
    slaveTimeStatus = "clock valid at startup";
  } else {
    slaveTimeSource = "unsynchronized";
    slaveTimeStatus = meshIsSlave()
        ? "clock invalid; awaiting master or Internet NTP"
        : "clock not yet synchronized";
  }

  xTaskCreatePinnedToCore(
      slaveTimeTask,
      "slaveTime",
      8192,
      nullptr,
      1,
      &slaveTimeTaskHandle,
      0);
}
