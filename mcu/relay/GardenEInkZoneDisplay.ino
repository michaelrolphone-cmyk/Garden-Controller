#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// Required hardware target: 7.5-inch 800x480 GDEY075T7 black/white panel.
// Target board pinout supplied for this hardware build.
static const uint8_t EPD_MOSI_PIN = 4;
static const uint8_t EPD_SCLK_PIN = 5;
static const uint8_t EPD_CS_PIN   = 7;
static const uint8_t EPD_DC_PIN   = 3;
static const uint8_t EPD_RST_PIN  = 6;
static const uint8_t EPD_BUSY_PIN = 2;
static const uint8_t SD_CS_PIN    = 20;
static const uint8_t SD_MISO_PIN  = 19;

GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> display(
  GxEPD2_750_GDEY075T7(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)
);

WebServer server(80);
DNSServer dns;
Preferences prefs;

struct ZoneCfg {
  char name[24];
  uint16_t baseMinutes;
  uint8_t startHour;
  uint8_t startMinute;
};

struct WeatherNow {
  char summary[48];
  char condition[32];
  float temperatureF;
  float humidityPct;
  float dewPointF;
  float precipitationChancePct;
  float windMph;
  int windDeg;
  char windDirection[8];
  float rainIn;
  float sunlightHours;
  unsigned long sunriseEpoch;
  unsigned long sunsetEpoch;
};

struct RunState {
  bool active;
  uint8_t zone;
  uint16_t remainingSeconds;
  uint16_t totalSeconds;
};

struct ScheduleSlot {
  bool loaded;
  uint8_t startHour;
  uint8_t startMinute;
  uint16_t runMinutes;
};

struct Pt { int16_t x; int16_t y; };

// Keep MapPt near the top of the .ino so Arduino's generated prototypes can see it.
struct MapPt { float x; float y; };

static const uint8_t DISPLAY_ZONE_COUNT = 5;
static const uint8_t MAP_POLY_COUNT = 6;

struct DisplayState {
  char title[64];
  char date[32];
  char time[16];
  bool masterEnable;
  bool weatherAdjustmentEnabled;
  bool weatherLoaded;
  char gardenNews[512];
  char displayMode[16];
  ZoneCfg zones[DISPLAY_ZONE_COUNT];
  bool scheduleLoaded;
  bool scheduleZoneLoaded[DISPLAY_ZONE_COUNT];
  ScheduleSlot scheduleSlots[DISPLAY_ZONE_COUNT][2];
  bool activeZones[DISPLAY_ZONE_COUNT];
  uint16_t activeRemainingSeconds[DISPLAY_ZONE_COUNT];
  uint16_t activeTotalSeconds[DISPLAY_ZONE_COUNT];
  bool spigotsOn;
  WeatherNow weather;
  RunState run;
};

struct HistoryRow {
  unsigned long epoch;
  float tempF;
  float rainIn;
  float sunlightHours;
  float windMph;
  int weatherCode;
  char reason[24];
};

static const char* HISTORY_CSV_DATA =
  "epoch,tempF,rainIn,sunlightHours,windMph,weatherCode,reason\n"
  "1715083200,62.3,0.00,9.4,4.1,1000,baseline\n"
  "1715169600,65.1,0.12,7.8,6.0,1003,spring-rain\n"
  "1715256000,69.8,0.00,10.2,5.4,1000,clear\n"
  "1715342400,72.4,0.08,11.3,7.2,1006,breezy\n"
  "1715428800,68.2,0.25,6.5,9.1,1009,storm\n";

DisplayState state = {};
DisplayState lastDrawn = {};

char apSsid[32] = "GardenEInkDisplay";
char apPass[64] = "gardenpaper";
char staSsid[32] = "";
char staPass[64] = "";
char relayBase[128] = "http://192.168.4.1";
char relayApiToken[96] = "";

char bootStatus[256] = "Booting";
char relayStatus[384] = "Relay has not been contacted yet.";
char relayLastError[384] = "";
char relayLastStage[48] = "Boot";
bool relayDataReady = false;
bool forceDiagnosticScreen = true;

unsigned long lastPollMs = 0;
unsigned long rotationEpochMs = 0;
bool forceFullRedraw = true;
bool queueStopped = false;
int queueDepth = 0;
float zoneLedger[DISPLAY_ZONE_COUNT] = {0, 0, 0, 0, 0};
int pendingExtraZone = 0;
int pendingExtraMinutes = 0;
uint8_t lastFinishedZone = 0;

static const unsigned long RELAY_STATE_POLL_MS = 2000UL;
static const unsigned long RELAY_FULL_SYNC_MS = 60000UL;
static const unsigned long RELAY_RETRY_SYNC_MS = 10000UL;
unsigned long lastFullSyncMs = 0;
static const unsigned long RUNTIME_ZONE_ROTATE_MS = 12000UL;
static const unsigned long RUNTIME_ZONE_FLASH_MS = 500UL;
uint8_t lastRuntimeMeterZone = 0;
uint8_t runtimeSwitchFlashZone = 0;
unsigned long runtimeSwitchFlashUntilMs = 0;
bool runtimeSwitchFlashVisible = false;
uint8_t runtimeSwitchFlashLastPhase = 255;
String lastHeaderDateTimeDrawn = "";

const char* MODE_AUTO = "auto";
const char* MODE_SCHEDULE = "schedule";
const char* MODE_NEWS = "news";
const char* MODE_GRAPH = "graph";

// Temporary field-test mode: keep the e-ink display on the schedule/runtime
// screen only. News/weather full-screen rotation remains compiled in, but is
// disabled until this flag is set false again.
static const bool SCHEDULE_SCREEN_ONLY = true;

String wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAILABLE";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return String("UNKNOWN_") + String((int)s);
  }
}

void setBootStatus(const char* msg) {
  strlcpy(bootStatus, msg, sizeof(bootStatus));
  Serial.println(msg);
}

void setRelayStatus(const char* stage, const String& msg, bool ok) {
  strlcpy(relayLastStage, stage, sizeof(relayLastStage));
  strlcpy(relayStatus, msg.c_str(), sizeof(relayStatus));
  if (ok) {
    relayLastError[0] = '\0';
  } else {
    strlcpy(relayLastError, msg.c_str(), sizeof(relayLastError));
  }
  Serial.printf("[%s] %s\n", stage, msg.c_str());
}

String ordinalDay(int d) {
  String s = "th";
  if ((d % 100) < 11 || (d % 100) > 13) {
    if (d % 10 == 1) s = "st";
    else if (d % 10 == 2) s = "nd";
    else if (d % 10 == 3) s = "rd";
  }
  return String(d) + s;
}

bool updateHeaderClockFromSystem() {
  time_t now;
  time(&now);
  if (now <= 1700000000UL) return false;

  struct tm* tmv = localtime(&now);
  if (!tmv) return false;

  const char* weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

  char newDate[sizeof(state.date)];
  char newTime[sizeof(state.time)];
  snprintf(newDate, sizeof(newDate), "%s, %s %s", weekdays[tmv->tm_wday], months[tmv->tm_mon], ordinalDay(tmv->tm_mday).c_str());

  int h = tmv->tm_hour % 12;
  if (h == 0) h = 12;
  snprintf(newTime, sizeof(newTime), "%d:%02d %s", h, tmv->tm_min, tmv->tm_hour >= 12 ? "PM" : "AM");

  bool changed = strcmp(state.date, newDate) != 0 || strcmp(state.time, newTime) != 0;
  if (changed) {
    strlcpy(state.date, newDate, sizeof(state.date));
    strlcpy(state.time, newTime, sizeof(state.time));
  }
  return changed;
}

String currentHeaderDateTime() {
  return String(state.date) + "  " + String(state.time);
}

void drawHeaderClockTextOnly() {
  display.fillRect(420, 0, 372, 45, GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);
  String headerDateTime = currentHeaderDateTime();
  int16_t tbx = 0;
  int16_t tby = 0;
  uint16_t tbw = 0;
  uint16_t tbh = 0;
  display.getTextBounds(headerDateTime, 0, 25, &tbx, &tby, &tbw, &tbh);
  int16_t headerX = 792 - (int16_t)tbw;
  if (headerX < 420) headerX = 420;
  display.setCursor(headerX, 25);
  display.print(headerDateTime);
  lastHeaderDateTimeDrawn = headerDateTime;
}

void partialUpdateHeaderClock() {
  display.setPartialWindow(420, 0, 372, 45);
  display.firstPage();
  do {
    drawHeaderClockTextOnly();
  } while (display.nextPage());
}

uint8_t activeZoneCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    if (state.activeZones[i]) count++;
  }
  return count;
}

uint8_t runtimeMeterZone() {
  uint8_t count = activeZoneCount();
  if (count == 0) return 0;
  uint8_t pick = (millis() / RUNTIME_ZONE_ROTATE_MS) % count;
  for (uint8_t i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    if (!state.activeZones[i]) continue;
    if (pick == 0) return i + 1;
    pick--;
  }
  return state.run.zone;
}

bool isRuntimeZoneFlashActive(uint8_t displayZone) {
  return runtimeSwitchFlashVisible &&
         runtimeSwitchFlashZone == displayZone &&
         (int32_t)(millis() - runtimeSwitchFlashUntilMs) < 0;
}

uint8_t runtimeZoneFlashPhase() {
  if (!runtimeSwitchFlashVisible || (int32_t)(millis() - runtimeSwitchFlashUntilMs) >= 0) return 255;
  unsigned long remaining = runtimeSwitchFlashUntilMs - millis();
  unsigned long elapsed = RUNTIME_ZONE_FLASH_MS > remaining ? RUNTIME_ZONE_FLASH_MS - remaining : 0;
  uint8_t phase = (uint8_t)((elapsed * 4UL) / max(1UL, RUNTIME_ZONE_FLASH_MS));
  return phase > 3 ? 3 : phase;
}

bool isRuntimeZoneFlashBadgeInverted(uint8_t displayZone) {
  if (!isRuntimeZoneFlashActive(displayZone)) return false;
  uint8_t phase = runtimeZoneFlashPhase();
  // Two quick badge inversions during the flash window. The active polygon
  // fill and outline stay stable; only the zone-number badge blinks.
  return phase == 0 || phase == 2;
}

void formatTimeLowerNoLeadingZero(unsigned long epoch, char* out, size_t outSize) {
  if (!epoch) { snprintf(out, outSize, "--:--"); return; }
  time_t t = (time_t)epoch;
  struct tm* tmv = localtime(&t);
  int hour = tmv->tm_hour % 12;
  if (hour == 0) hour = 12;
  snprintf(out, outSize, "%d:%02d%s", hour, tmv->tm_min, tmv->tm_hour >= 12 ? "pm" : "am");
}

float directionToDegrees(const char* dir) {
  if (!dir || !*dir) return 0.0f;
  if (strcmp(dir, "N") == 0) return 0.0f;
  if (strcmp(dir, "NE") == 0) return 45.0f;
  if (strcmp(dir, "E") == 0) return 90.0f;
  if (strcmp(dir, "SE") == 0) return 135.0f;
  if (strcmp(dir, "S") == 0) return 180.0f;
  if (strcmp(dir, "SW") == 0) return 225.0f;
  if (strcmp(dir, "W") == 0) return 270.0f;
  if (strcmp(dir, "NW") == 0) return 315.0f;
  return 0.0f;
}

void saveConfig() {
  prefs.begin("eink", false);
  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  prefs.putString("staSsid", staSsid);
  prefs.putString("staPass", staPass);
  prefs.putString("relayBase", relayBase);
  prefs.putString("relayApiToken", relayApiToken);
  prefs.putString("displayMode", state.displayMode);
  prefs.putString("gardenNews", state.gardenNews);
  prefs.end();
}

void loadConfig() {
  prefs.begin("eink", true);
  strlcpy(apSsid, prefs.getString("apSsid", "GardenEInkDisplay").c_str(), sizeof(apSsid));
  strlcpy(apPass, prefs.getString("apPass", "gardenpaper").c_str(), sizeof(apPass));
  strlcpy(staSsid, prefs.getString("staSsid", "").c_str(), sizeof(staSsid));
  strlcpy(staPass, prefs.getString("staPass", "").c_str(), sizeof(staPass));
  strlcpy(relayBase, prefs.getString("relayBase", "http://192.168.4.1").c_str(), sizeof(relayBase));
  strlcpy(relayApiToken, prefs.getString("relayApiToken", "").c_str(), sizeof(relayApiToken));
  strlcpy(state.displayMode, prefs.getString("displayMode", MODE_AUTO).c_str(), sizeof(state.displayMode));
  strlcpy(state.gardenNews, prefs.getString("gardenNews", "Welcome to Castle Hills Garden.").c_str(), sizeof(state.gardenNews));
  prefs.end();

  if (state.displayMode[0] == '\0') strlcpy(state.displayMode, MODE_AUTO, sizeof(state.displayMode));
  if (SCHEDULE_SCREEN_ONLY) strlcpy(state.displayMode, MODE_SCHEDULE, sizeof(state.displayMode));
  strlcpy(state.title, "Castle Hills Garden Schedule", sizeof(state.title));
  strlcpy(state.date, "No relay time", sizeof(state.date));
  strlcpy(state.time, "--:--", sizeof(state.time));

  state.scheduleLoaded = false;
  const char* defaults[DISPLAY_ZONE_COUNT] = {"Zone 1", "Zone 2", "Zone 3", "Zone 4", "Zone 5"};
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    state.activeZones[i] = false;
    state.activeRemainingSeconds[i] = 0;
    state.activeTotalSeconds[i] = 0;
    state.scheduleZoneLoaded[i] = false;
    for (int slot = 0; slot < 2; slot++) {
      state.scheduleSlots[i][slot].loaded = false;
      state.scheduleSlots[i][slot].startHour = 0;
      state.scheduleSlots[i][slot].startMinute = 0;
      state.scheduleSlots[i][slot].runMinutes = 0;
    }
    if (state.zones[i].name[0] == '\0') strlcpy(state.zones[i].name, defaults[i], sizeof(state.zones[i].name));
    // These defaults are only internal placeholders. The paper schedule panel
    // will show "Not Loaded" until a relay schedule payload is received.
    if (state.zones[i].baseMinutes == 0) state.zones[i].baseMinutes = 15;
  }
}

void setupWifi() {
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAPdisconnect(true);
  delay(100);

  IPAddress einkApIp(192, 168, 5, 1);
  IPAddress einkApGateway(192, 168, 5, 1);
  IPAddress einkApSubnet(255, 255, 255, 0);
  bool apConfigOk = WiFi.softAPConfig(einkApIp, einkApGateway, einkApSubnet);

  bool apOk = false;
  if (strlen(apPass) >= 8) apOk = WiFi.softAP(apSsid, apPass);
  else apOk = WiFi.softAP(apSsid);

  IPAddress apIp = WiFi.softAPIP();
  dns.stop();
  dns.start(53, "*", apIp);

  String msg = String("Admin AP ") + (apOk ? "started" : "failed") + ": " + apSsid + " at " + apIp.toString();
  if (!apConfigOk) msg += " (softAPConfig failed; expected 192.168.5.1)";
  setBootStatus(msg.c_str());

  WiFi.disconnect(false, false);
  delay(100);
  if (strlen(staSsid) > 0) {
    WiFi.begin(staSsid, staPass);
    setRelayStatus("Station WiFi", String("Connecting to station SSID: ") + staSsid, false);
  } else {
    setRelayStatus("Station WiFi", "No station SSID configured. Use the admin interface to set Wi-Fi.", false);
  }
}

bool waitForStationWifi(uint32_t timeoutMs) {
  if (strlen(staSsid) == 0) {
    setRelayStatus("Station WiFi", "No station SSID configured. Open the admin AP and save Wi-Fi settings.", false);
    return false;
  }

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
    dns.processNextRequest();
    server.handleClient();
  }

  if (WiFi.status() != WL_CONNECTED) {
    setRelayStatus("Station WiFi", String("Failed to connect to '") + staSsid + "'. WiFi.status=" + wifiStatusName(WiFi.status()) + ". Check SSID/password and signal.", false);
    return false;
  }

  setRelayStatus("Station WiFi", String("Connected to ") + staSsid + ". IP " + WiFi.localIP().toString(), true);
  return true;
}

void applyScreenRotationMode() {
  if (SCHEDULE_SCREEN_ONLY) {
    strlcpy(state.title, "Castle Hills Garden Schedule", sizeof(state.title));
    return;
  }

  // Treat relay-6/spigot/master-valve activity like watering activity for
  // display selection. Even if no map zone is running, the user needs the
  // live schedule/runtime page instead of the news or weather rotation page.
  if (state.run.active || state.spigotsOn) {
    strlcpy(state.title, "Castle Hills Garden Schedule", sizeof(state.title));
    return;
  }
  if (strcmp(state.displayMode, MODE_AUTO) != 0) {
    if (strcmp(state.displayMode, MODE_NEWS) == 0) strlcpy(state.title, "Castle Hills Garden News", sizeof(state.title));
    else if (strcmp(state.displayMode, MODE_GRAPH) == 0) strlcpy(state.title, "Current + Weekly Weather", sizeof(state.title));
    else strlcpy(state.title, "Castle Hills Garden Schedule", sizeof(state.title));
    return;
  }

  unsigned long cycleMs = (millis() - rotationEpochMs) % (4UL * 60UL * 1000UL);
  if (cycleMs < 45000UL) strlcpy(state.title, "Castle Hills Garden News", sizeof(state.title));
  else if (cycleMs >= 120000UL && cycleMs < 165000UL) strlcpy(state.title, "Current + Weekly Weather", sizeof(state.title));
  else strlcpy(state.title, "Castle Hills Garden Schedule", sizeof(state.title));
}

bool fetchRelayJson(const String& path, DynamicJsonDocument& out, const char* stage) {
  if (strlen(relayBase) == 0) {
    setRelayStatus(stage, "Relay base URL is blank. Save relay settings in the admin interface.", false);
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    setRelayStatus(stage, String("Cannot call ") + path + ": station Wi-Fi is not connected. WiFi.status=" + wifiStatusName(WiFi.status()), false);
    return false;
  }

  String url = String(relayBase) + path;
  HTTPClient http;
  http.setTimeout(6000);

  if (!http.begin(url)) {
    setRelayStatus(stage, String("HTTP begin failed for ") + url + ". Check relayBase URL format.", false);
    return false;
  }

  if (strlen(relayApiToken) > 0) {
    http.addHeader("x-api-token", relayApiToken);
  }

  int code = http.GET();
  String body = http.getString();
  if (code <= 0) {
    String msg = String("HTTP request failed for ") + url + ": " + http.errorToString(code);
    http.end();
    setRelayStatus(stage, msg, false);
    return false;
  }

  if (code != 200) {
    String excerpt = body.substring(0, 120);
    String msg = String("Relay returned HTTP ") + code + " for " + path + ". Body: " + excerpt;
    http.end();
    setRelayStatus(stage, msg, false);
    return false;
  }

  DeserializationError err = deserializeJson(out, body);
  http.end();

  if (err) {
    setRelayStatus(stage, String("JSON parse failed for ") + path + ": " + err.c_str(), false);
    return false;
  }

  setRelayStatus(stage, String("OK: ") + path, true);
  return true;
}

bool relayGetText(const String& path, const char* stage, String& bodyOut) {
  bodyOut = "";

  if (strlen(relayBase) == 0) {
    setRelayStatus(stage, "Relay base URL is blank. Save relay settings in the admin interface.", false);
    return false;
  }

  if (!waitForStationWifi(3000)) {
    setRelayStatus(stage, String("Cannot call ") + path + ": station Wi-Fi is not connected. WiFi.status=" + wifiStatusName(WiFi.status()), false);
    return false;
  }

  String url = String(relayBase) + path;
  HTTPClient http;
  http.setTimeout(6000);

  if (!http.begin(url)) {
    setRelayStatus(stage, String("HTTP begin failed for ") + url + ". Check relayBase URL format.", false);
    return false;
  }

  if (strlen(relayApiToken) > 0) {
    http.addHeader("x-api-token", relayApiToken);
  }

  int code = http.GET();
  bodyOut = http.getString();

  if (code <= 0) {
    String msg = String("HTTP request failed for ") + url + ": " + http.errorToString(code);
    http.end();
    setRelayStatus(stage, msg, false);
    return false;
  }

  if (code != 200) {
    String excerpt = bodyOut.substring(0, 120);
    String msg = String("Relay returned HTTP ") + code + " for " + path + ". Body: " + excerpt;
    http.end();
    setRelayStatus(stage, msg, false);
    return false;
  }

  http.end();
  setRelayStatus(stage, String("OK: ") + path, true);
  return true;
}

bool fetchRunState(DynamicJsonDocument& sdoc) {
  // Relay firmware route table exposes the canonical state endpoint at /api/state.
  // /status exists as an alias on the relay, but the display should prefer /api/state
  // so all relay API calls are consistently namespaced except /time and /weather.
  DynamicJsonDocument first(8192);
  if (fetchRelayJson("/api/state", first, "Relay state /api/state")) {
    sdoc.clear();
    sdoc.set(first.as<JsonVariant>());
    return true;
  }

  char firstError[384];
  strlcpy(firstError, relayLastError, sizeof(firstError));

  DynamicJsonDocument second(8192);
  if (fetchRelayJson("/status", second, "Relay state /status fallback")) {
    sdoc.clear();
    sdoc.set(second.as<JsonVariant>());
    return true;
  }

  setRelayStatus("Relay status", String("Both state endpoints failed. /api/state: ") + firstError + " /status: " + relayLastError, false);
  return false;
}

static bool jsonBool(JsonVariant v, bool fallback = false) {
  if (v.isNull()) return fallback;
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<int>()) return v.as<int>() != 0;
  const char* s = v.as<const char*>();
  if (!s) return fallback;
  String t(s);
  t.trim();
  t.toLowerCase();
  return t == "1" || t == "true" || t == "on" || t == "yes" || t == "active";
}

static uint32_t jsonU32(JsonVariant v, uint32_t fallback = 0) {
  if (v.isNull()) return fallback;
  if (v.is<unsigned long>()) return v.as<unsigned long>();
  if (v.is<long>()) {
    long n = v.as<long>();
    return n < 0 ? fallback : (uint32_t)n;
  }
  if (v.is<int>()) {
    int n = v.as<int>();
    return n < 0 ? fallback : (uint32_t)n;
  }
  const char* s = v.as<const char*>();
  if (!s || !*s) return fallback;
  return (uint32_t)strtoul(s, nullptr, 10);
}

static uint32_t firstU32(JsonObject obj, const char* const* keys, uint8_t count, uint32_t fallback = 0) {
  for (uint8_t i = 0; i < count; i++) {
    if (!obj[keys[i]].isNull()) return jsonU32(obj[keys[i]], fallback);
  }
  return fallback;
}

static uint8_t normalizeDisplayZoneValue(int raw, bool rawIsZeroBased) {
  if (rawIsZeroBased) {
    if (raw >= 0 && raw < DISPLAY_ZONE_COUNT) return (uint8_t)(raw + 1);
    return 0;
  }
  if (raw >= 1 && raw <= DISPLAY_ZONE_COUNT) return (uint8_t)raw;
  if (raw == 0) return 1; // defensive: relay/currentRun.zone has appeared as zero-based
  return 0;
}

static uint8_t zoneFromObject(JsonObject obj, int fallbackIndex = -1) {
  // Explicit display/number fields are one-based.
  const char* oneBasedKeys[] = {"zoneNumber", "displayZone", "activeZoneNumber", "zoneDisplay", "zoneNo"};
  for (uint8_t i = 0; i < sizeof(oneBasedKeys) / sizeof(oneBasedKeys[0]); i++) {
    if (!obj[oneBasedKeys[i]].isNull()) {
      uint8_t z = normalizeDisplayZoneValue(obj[oneBasedKeys[i]].as<int>(), false);
      if (z) return z;
    }
  }

  // Index fields are zero-based in GardenSimpleRelay6.
  const char* zeroBasedKeys[] = {"zoneIndex", "idx", "index"};
  for (uint8_t i = 0; i < sizeof(zeroBasedKeys) / sizeof(zeroBasedKeys[0]); i++) {
    if (!obj[zeroBasedKeys[i]].isNull()) {
      uint8_t z = normalizeDisplayZoneValue(obj[zeroBasedKeys[i]].as<int>(), true);
      if (z) return z;
    }
  }

  // Relay/channel fields are normally one-based. Treat 0 defensively as Zone 1.
  const char* relayKeys[] = {"relay", "relayChannel", "channel"};
  for (uint8_t i = 0; i < sizeof(relayKeys) / sizeof(relayKeys[0]); i++) {
    if (!obj[relayKeys[i]].isNull()) {
      uint8_t z = normalizeDisplayZoneValue(obj[relayKeys[i]].as<int>(), false);
      if (z) return z;
    }
  }

  // The ambiguous field "zone" has appeared as zero-based in currentRun.
  if (!obj["zone"].isNull()) {
    int raw = obj["zone"].as<int>();
    if (raw == 0) return 1;
    if (raw >= 1 && raw <= DISPLAY_ZONE_COUNT) return (uint8_t)raw;
  }

  if (fallbackIndex >= 0 && fallbackIndex < DISPLAY_ZONE_COUNT) return (uint8_t)(fallbackIndex + 1);
  return 0;
}

static uint32_t durationSecondsFromObject(JsonObject obj) {
  const char* secKeys[] = {"durationSeconds", "totalSeconds", "durationSec", "runSeconds", "seconds", "duration"};
  uint32_t sec = firstU32(obj, secKeys, sizeof(secKeys) / sizeof(secKeys[0]), 0);
  if (sec > 0) return sec;
  const char* msKeys[] = {"durationMs", "totalMs", "runMs"};
  uint32_t ms = firstU32(obj, msKeys, sizeof(msKeys) / sizeof(msKeys[0]), 0);
  return ms > 0 ? (ms + 999UL) / 1000UL : 0;
}

static uint32_t remainingSecondsFromObject(JsonObject obj) {
  const char* secKeys[] = {"remainingSeconds", "remainingSec", "remaining", "secondsRemaining", "secondsLeft", "leftSeconds"};
  uint32_t sec = firstU32(obj, secKeys, sizeof(secKeys) / sizeof(secKeys[0]), 0);
  if (sec > 0) return sec;
  const char* msKeys[] = {"remainingMs", "leftMs"};
  uint32_t ms = firstU32(obj, msKeys, sizeof(msKeys) / sizeof(msKeys[0]), 0);
  if (ms > 0) return (ms + 999UL) / 1000UL;

  uint32_t durationSec = durationSecondsFromObject(obj);
  const char* elapsedSecKeys[] = {"elapsedSeconds", "elapsedSec"};
  uint32_t elapsedSec = firstU32(obj, elapsedSecKeys, sizeof(elapsedSecKeys) / sizeof(elapsedSecKeys[0]), 0);
  if (elapsedSec == 0) {
    const char* elapsedMsKeys[] = {"elapsedMs"};
    uint32_t elapsedMs = firstU32(obj, elapsedMsKeys, sizeof(elapsedMsKeys) / sizeof(elapsedMsKeys[0]), 0);
    if (elapsedMs > 0) elapsedSec = elapsedMs / 1000UL;
  }
  if (durationSec > elapsedSec) return durationSec - elapsedSec;
  return 0;
}

static bool relayRunObjectActive(JsonObject obj) {
  if (obj.isNull()) return false;
  if (jsonBool(obj["active"], false)) return true;
  if (jsonBool(obj["running"], false)) return true;
  if (jsonBool(obj["relayOn"], false)) return true;
  if (jsonBool(obj["on"], false)) return true;
  if (jsonBool(obj["state"], false)) return true;
  return false;
}

static bool localRunTimerActive = false;
static uint8_t localRunTimerZone = 0;
static uint32_t localRunTimerStartMs = 0;
static uint32_t localRunTimerTotalSeconds = 0;

static void finalizeRunTimer(bool active, uint8_t zone, uint32_t& remainingSeconds, uint32_t& totalSeconds) {
  if (!active || zone == 0) {
    localRunTimerActive = false;
    localRunTimerZone = 0;
    localRunTimerTotalSeconds = 0;
    return;
  }

  if (totalSeconds == 0 && remainingSeconds > 0) totalSeconds = remainingSeconds;

  bool newRun = !localRunTimerActive || localRunTimerZone != zone ||
                (totalSeconds > 0 && localRunTimerTotalSeconds != totalSeconds);

  if (newRun) {
    localRunTimerActive = true;
    localRunTimerZone = zone;
    localRunTimerTotalSeconds = totalSeconds;
    if (totalSeconds > 0 && remainingSeconds > 0 && remainingSeconds <= totalSeconds) {
      localRunTimerStartMs = millis() - ((totalSeconds - remainingSeconds) * 1000UL);
    } else {
      localRunTimerStartMs = millis();
    }
  }

  if (remainingSeconds == 0 && localRunTimerActive && localRunTimerTotalSeconds > 0) {
    uint32_t elapsed = (millis() - localRunTimerStartMs) / 1000UL;
    remainingSeconds = elapsed >= localRunTimerTotalSeconds ? 0 : localRunTimerTotalSeconds - elapsed;
    totalSeconds = localRunTimerTotalSeconds;
  }
}


static bool extractScheduleFields(JsonObject src, uint8_t& zoneOut, uint8_t& hourOut, uint8_t& minuteOut, uint16_t& minutesOut) {
  int rawZone = -999;
  bool zoneIsZeroBased = true;
  if (!src["zoneIndex"].isNull()) { rawZone = src["zoneIndex"].as<int>(); zoneIsZeroBased = true; }
  else if (!src["index"].isNull()) { rawZone = src["index"].as<int>(); zoneIsZeroBased = true; }
  else if (!src["idx"].isNull()) { rawZone = src["idx"].as<int>(); zoneIsZeroBased = true; }
  else if (!src["zoneNumber"].isNull()) { rawZone = src["zoneNumber"].as<int>(); zoneIsZeroBased = false; }
  else if (!src["zone"].isNull()) {
    rawZone = src["zone"].as<int>();
    // GardenSimpleRelay6 buildStateJson() emits dailySchedules[].zone as one-based.
    // Treat bare schedule "zone" as display zone number here; using zero-based here
    // shifts every schedule down one row (Zone 2 shows Zone 1's schedule).
    zoneIsZeroBased = false;
  }
  if (rawZone == -999) return false;
  uint8_t z = normalizeDisplayZoneValue(rawZone, zoneIsZeroBased);
  if (z < 1 || z > DISPLAY_ZONE_COUNT) return false;

  int h = -1;
  int m = -1;
  if (!src["startHour"].isNull()) h = src["startHour"].as<int>();
  else if (!src["hour"].isNull()) h = src["hour"].as<int>();
  if (!src["startMinute"].isNull()) m = src["startMinute"].as<int>();
  else if (!src["minute"].isNull()) m = src["minute"].as<int>();

  const char* timeKeys[] = {"startTime", "time"};
  for (uint8_t i = 0; (h < 0 || m < 0) && i < sizeof(timeKeys) / sizeof(timeKeys[0]); i++) {
    const char* t = src[timeKeys[i]] | nullptr;
    if (t && strlen(t) >= 4) {
      int hh = atoi(t);
      const char* colon = strchr(t, ':');
      if (colon) {
        int mm = atoi(colon + 1);
        h = hh;
        m = mm;
      }
    }
  }
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;

  uint32_t minutes = 0;
  const char* minKeys[] = {"runMinutes", "baseMinutes", "minutes", "durationMinutes", "durationMin"};
  minutes = firstU32(src, minKeys, sizeof(minKeys) / sizeof(minKeys[0]), 0);
  if (minutes == 0) {
    const char* secKeys[] = {"durationSeconds", "runSeconds", "seconds"};
    uint32_t seconds = firstU32(src, secKeys, sizeof(secKeys) / sizeof(secKeys[0]), 0);
    if (seconds > 0) minutes = (seconds + 59UL) / 60UL;
  }
  if (minutes == 0) return false;

  zoneOut = z;
  hourOut = (uint8_t)h;
  minuteOut = (uint8_t)m;
  minutesOut = (uint16_t)constrain((int)minutes, 1, 240);
  return true;
}

static void clearScheduleSlots() {
  state.scheduleLoaded = false;
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    state.scheduleZoneLoaded[i] = false;
    for (int slot = 0; slot < 2; slot++) {
      state.scheduleSlots[i][slot].loaded = false;
      state.scheduleSlots[i][slot].startHour = 0;
      state.scheduleSlots[i][slot].startMinute = 0;
      state.scheduleSlots[i][slot].runMinutes = 0;
    }
  }
}

static bool addScheduleSlot(uint8_t displayZone, uint8_t h, uint8_t m, uint16_t mins) {
  if (displayZone < 1 || displayZone > DISPLAY_ZONE_COUNT) return false;
  uint8_t idx = displayZone - 1;
  for (int slot = 0; slot < 2; slot++) {
    if (!state.scheduleSlots[idx][slot].loaded) {
      state.scheduleSlots[idx][slot].loaded = true;
      state.scheduleSlots[idx][slot].startHour = h;
      state.scheduleSlots[idx][slot].startMinute = m;
      state.scheduleSlots[idx][slot].runMinutes = mins;
      state.scheduleZoneLoaded[idx] = true;
      state.scheduleLoaded = true;
      if (slot == 0) {
        state.zones[idx].startHour = h;
        state.zones[idx].startMinute = m;
        state.zones[idx].baseMinutes = mins;
      }
      return true;
    }
  }
  return false;
}

static bool applySchedulesFromArray(JsonArray arr) {
  if (arr.isNull() || arr.size() == 0) return false;

  bool loadedAny = false;
  for (JsonObject item : arr) {
    if (item.isNull()) continue;
    if (!item["enabled"].isNull() && !jsonBool(item["enabled"], true)) continue;
    uint8_t z = 0, h = 0, m = 0;
    uint16_t mins = 0;
    if (!extractScheduleFields(item, z, h, m, mins)) continue;
    if (addScheduleSlot(z, h, m, mins)) loadedAny = true;
  }

  return loadedAny;
}

static bool applyScheduleFromRelayJson(JsonVariant root) {
  clearScheduleSlots();
  bool loaded = false;

  JsonArray schedules = root["dailySchedules"].as<JsonArray>();
  if (!schedules.isNull()) loaded = applySchedulesFromArray(schedules) || loaded;
  schedules = root["schedules"].as<JsonArray>();
  if (!schedules.isNull()) loaded = applySchedulesFromArray(schedules) || loaded;

  JsonArray zones = root["zones"].as<JsonArray>();
  if (!zones.isNull()) {
    int i = 0;
    bool zonesHadSchedule = false;
    for (JsonObject zobj : zones) {
      if (i >= DISPLAY_ZONE_COUNT) break;
      if (!zobj["name"].isNull()) strlcpy(state.zones[i].name, zobj["name"] | state.zones[i].name, sizeof(state.zones[i].name));
      uint8_t z = 0, h = 0, m = 0;
      uint16_t mins = 0;
      if (extractScheduleFields(zobj, z, h, m, mins)) {
        if (addScheduleSlot(z, h, m, mins)) zonesHadSchedule = true;
      }
      i++;
    }
    if (zonesHadSchedule) {
      state.scheduleLoaded = true;
      loaded = true;
    }
  }

  return loaded;
}

uint8_t normalizeRelayZoneFromRun(JsonObject run, JsonVariant root) {
  uint8_t z = zoneFromObject(run, -1);
  if (z) return z;

  JsonArray zoneRuns = root["zoneRuns"].as<JsonArray>();
  if (!zoneRuns.isNull()) {
    int idx = 0;
    for (JsonObject zr : zoneRuns) {
      if (relayRunObjectActive(zr)) {
        z = zoneFromObject(zr, idx);
        if (z) return z;
      }
      idx++;
    }
  }

  JsonArray relays = root["relays"].as<JsonArray>();
  if (!relays.isNull()) {
    int idx = 0;
    for (JsonObject r : relays) {
      bool on = jsonBool(r["on"], false) || jsonBool(r["state"], false) || jsonBool(r["active"], false);
      if (on && idx < DISPLAY_ZONE_COUNT) return (uint8_t)(idx + 1);
      idx++;
    }
  }

  return 0;
}

void applyRunStateFromRelayJson(DynamicJsonDocument& sdoc) {
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    state.activeZones[i] = false;
    state.activeRemainingSeconds[i] = 0;
    state.activeTotalSeconds[i] = 0;
  }
  state.spigotsOn = false;

  JsonVariant root = sdoc.as<JsonVariant>();
  applyScheduleFromRelayJson(root);

  bool anyActive = false;
  uint8_t primaryZone = 0;
  uint32_t primaryRemaining = 0;
  uint32_t primaryTotal = 0;

  JsonObject run = sdoc["currentRun"].as<JsonObject>();
  if (relayRunObjectActive(run)) {
    uint8_t z = zoneFromObject(run, -1);
    if (z == 0) z = normalizeRelayZoneFromRun(run, root);
    if (z == 0) z = 1;
    if (z >= 1 && z <= DISPLAY_ZONE_COUNT) {
      anyActive = true;
      primaryZone = z;
      state.activeZones[z - 1] = true;
      primaryRemaining = remainingSecondsFromObject(run);
      primaryTotal = durationSecondsFromObject(run);
      state.activeRemainingSeconds[z - 1] = (uint16_t)min(primaryRemaining, 65535UL);
      state.activeTotalSeconds[z - 1] = (uint16_t)min(primaryTotal, 65535UL);
    }
  }

  JsonArray zoneRuns = sdoc["zoneRuns"].as<JsonArray>();
  if (!zoneRuns.isNull()) {
    int idx = 0;
    for (JsonObject zr : zoneRuns) {
      if (relayRunObjectActive(zr)) {
        uint8_t displayZone = zoneFromObject(zr, idx);
        if (displayZone >= 1 && displayZone <= DISPLAY_ZONE_COUNT) {
          anyActive = true;
          state.activeZones[displayZone - 1] = true;
          uint32_t zrRemaining = remainingSecondsFromObject(zr);
          uint32_t zrTotal = durationSecondsFromObject(zr);
          finalizeRunTimer(true, displayZone, zrRemaining, zrTotal);
          state.activeRemainingSeconds[displayZone - 1] = (uint16_t)min(zrRemaining, 65535UL);
          state.activeTotalSeconds[displayZone - 1] = (uint16_t)min(zrTotal, 65535UL);
          if (primaryZone == 0 || zrRemaining > primaryRemaining) {
            primaryZone = displayZone;
            primaryRemaining = zrRemaining;
            primaryTotal = zrTotal;
          }
        }
      }
      idx++;
    }
  }

  JsonArray relays = sdoc["relays"].as<JsonArray>();
  if (!relays.isNull()) {
    int idx = 0;
    for (JsonObject r : relays) {
      bool on = jsonBool(r["on"], false) || jsonBool(r["state"], false) || jsonBool(r["active"], false) || jsonBool(r["relayOn"], false);
      if (on && idx < DISPLAY_ZONE_COUNT) {
        anyActive = true;
        state.activeZones[idx] = true;
        if (primaryZone == 0) primaryZone = idx + 1;
      } else if (on && idx == 5) {
        // Relay 6 is the spigot/master valve channel. Show this only when
        // no watering zones are active. It is not a map watering zone.
        state.spigotsOn = true;
      }
      idx++;
    }
  }

  JsonObject spigotRunObj = sdoc["spigotRun"].as<JsonObject>();
  if (relayRunObjectActive(spigotRunObj)) state.spigotsOn = true;
  if (jsonBool(sdoc["spigotsOn"], false) || jsonBool(sdoc["spigotOn"], false) || jsonBool(sdoc["masterValveOn"], false)) state.spigotsOn = true;

  if (!anyActive && primaryZone > 0) anyActive = true;
  if (anyActive && primaryZone == 0) primaryZone = 1;

  finalizeRunTimer(anyActive, primaryZone, primaryRemaining, primaryTotal);
  if (anyActive && primaryZone >= 1 && primaryZone <= DISPLAY_ZONE_COUNT && state.activeRemainingSeconds[primaryZone - 1] == 0) {
    state.activeRemainingSeconds[primaryZone - 1] = (uint16_t)min(primaryRemaining, 65535UL);
    state.activeTotalSeconds[primaryZone - 1] = (uint16_t)min(primaryTotal, 65535UL);
  }

  state.run.active = anyActive;
  state.run.zone = anyActive ? primaryZone : 0;
  state.run.remainingSeconds = anyActive ? primaryRemaining : 0;
  state.run.totalSeconds = anyActive ? primaryTotal : 0;
}

bool syncRunStateOnly() {
  if (!waitForStationWifi(1200)) return false;
  DynamicJsonDocument sdoc(8192);
  if (!fetchRunState(sdoc)) {
    relayDataReady = false;
    forceDiagnosticScreen = true;
    forceFullRedraw = true;
    return false;
  }
  applyRunStateFromRelayJson(sdoc);
  relayDataReady = true;
  forceDiagnosticScreen = false;
  setRelayStatus("Relay state poll", "Runtime state updated from relay.", true);
  return true;
}

bool syncFromRelay() {
  relayDataReady = false;
  forceDiagnosticScreen = true;

  if (!waitForStationWifi(3000)) return false;

  DynamicJsonDocument tdoc(512);
  if (!fetchRelayJson("/time", tdoc, "Relay time")) return false;

  unsigned long epoch = tdoc["epoch"] | 0;
  if (epoch == 0) {
    setRelayStatus("Relay time", "Relay /time returned epoch=0. Time endpoint is responding but not synced.", false);
    return false;
  }

  setenv("TZ", "MST7MDT,M3.2.0,M11.1.0", 1);
  tzset();
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  updateHeaderClockFromSystem();

  DynamicJsonDocument wdoc(4096);
  if (!fetchRelayJson("/weather", wdoc, "Relay weather")) return false;

  strlcpy(state.weather.summary, wdoc["summary"] | "", sizeof(state.weather.summary));
  strlcpy(state.weather.condition, wdoc["condition"] | "", sizeof(state.weather.condition));

  // The relay currently uses summary="Updated" as a default/status marker even
  // when Open-Meteo has not actually populated the weather snapshot. Treat that
  // as a weather-load failure so the paper display does not present fake 0F data.
  state.weatherLoaded = true;
  if (strcmp(state.weather.summary, "Updated") == 0) {
    state.weatherLoaded = false;
  }

  state.weather.temperatureF = wdoc["temperatureF"] | 0;
  state.weather.humidityPct = wdoc["humidityPct"] | 0;
  state.weather.dewPointF = wdoc["dewPointF"] | 0;
  state.weather.precipitationChancePct = wdoc["precipitationChancePct"] | 0;
  state.weather.windMph = wdoc["windMph"] | 0;
  state.weather.windDeg = wdoc["windDeg"] | 0;
  strlcpy(state.weather.windDirection, wdoc["windDirection"] | "N", sizeof(state.weather.windDirection));
  state.weather.rainIn = wdoc["rainIn"] | 0;
  state.weather.sunlightHours = wdoc["sunlightHours"] | 0;
  state.weather.sunriseEpoch = wdoc["sunriseEpoch"] | 0;
  state.weather.sunsetEpoch = wdoc["sunsetEpoch"] | 0;

  DynamicJsonDocument sdoc(8192);
  if (!fetchRunState(sdoc)) return false;

  applyRunStateFromRelayJson(sdoc);

  relayDataReady = true;
  forceDiagnosticScreen = false;
  setRelayStatus("Relay data", "Relay data rendered successfully. Time, weather, and status are available.", true);
  return true;
}

// Castle Hills map geometry converted from the simulator/SVG viewBox.
// ViewBox: 0 0 295.743 295.482. The coordinates are scaled into the
// 424x424 map frame so the native e-paper map has the same zone shapes as
// the simulated paper display.
static const float MAP_VB_W = 295.743f;
static const float MAP_VB_H = 295.482f;

static const MapPt ZONE_POLY_1[] = {{127.534f,159.189f}, {124.128f,239.478f}, {15.618f,232.055f}, {22.680f,152.017f}, {127.534f,159.189f}};
static const MapPt ZONE_POLY_2[] = {{146.876f,95.990f}, {134.762f,166.150f}, {198.935f,169.661f}, {205.581f,96.842f}, {146.876f,95.990f}};
static const MapPt ZONE_POLY_3[] = {{205.581f,96.842f}, {264.287f,97.694f}, {263.108f,173.173f}, {198.935f,169.661f}, {205.581f,96.842f}};
static const MapPt ZONE_POLY_4[] = {{32.000f,46.388f}, {46.493f,52.635f}, {95.676f,139.247f}, {128.342f,140.128f}, {127.534f,159.189f}, {22.680f,152.017f}, {32.000f,46.388f}};
static const MapPt ZONE_POLY_5[] = {{152.903f,89.893f}, {152.691f,13.195f}, {50.949f,12.060f}, {136.836f,89.869f}, {152.903f,89.893f}};
static const MapPt ZONE_POLY_6[] = {{249.304f,90.041f}, {249.095f,14.271f}, {152.691f,13.195f}, {152.903f,89.893f}, {249.304f,90.041f}};

struct MapPolyRef { const MapPt* p; uint8_t n; };
static const MapPolyRef MAP_POLYS[MAP_POLY_COUNT] = {
  {ZONE_POLY_1, 5},
  {ZONE_POLY_2, 5},
  {ZONE_POLY_3, 5},
  {ZONE_POLY_4, 7},
  {ZONE_POLY_5, 5},
  {ZONE_POLY_6, 5},
};

// The relay firmware has five watering zones and relay channel 6 is the
// master/spigot channel, not a map watering zone. The SVG has six drawn
// polygons because watering Zone 4 is split into two separate top polygons.
// Keep these groups aligned to the relay's displayed Zone 1..5 numbering.
// Watering-zone to SVG-polygon mapping.
// Polygon indexes are MAP_POLYS[] order:
//   0 = bottom-left polygon
//   1 = middle polygon
//   2 = right-most polygon
//   3 = upper-left-side / diagonal polygon
//   4 = top-left polygon
//   5 = top-right-most polygon
// User-confirmed mapping:
//   Zone 1 = right-most polygon
//   Zone 2 = top-right-most polygon
//   Zone 3 = top-left polygon
//   Zone 4 = bottom-left polygon + middle polygon
//   Zone 5 = upper-most left-side polygon, not the top-left polygon
static const uint8_t ZONE1_POLYS[] = {2};       // right-most polygon
static const uint8_t ZONE2_POLYS[] = {5};       // top-right-most polygon
static const uint8_t ZONE3_POLYS[] = {4};       // top-left polygon
static const uint8_t ZONE4_POLYS[] = {0, 1};    // bottom-left + middle polygons make one zone
static const uint8_t ZONE5_POLYS[] = {3};       // upper-most left-side / diagonal polygon

struct ZoneMapGroup { const uint8_t* polyIndexes; uint8_t count; };
static const ZoneMapGroup DISPLAY_ZONE_MAP[DISPLAY_ZONE_COUNT] = {
  {ZONE1_POLYS, 1},
  {ZONE2_POLYS, 1},
  {ZONE3_POLYS, 1},
  {ZONE4_POLYS, 2},
  {ZONE5_POLYS, 1},
};

static inline int mapX(int frameX, int frameW, float vx) {
  return frameX + 6 + (int)roundf((vx / MAP_VB_W) * (frameW - 12));
}

static inline int mapY(int frameY, int frameH, float vy) {
  return frameY + 6 + (int)roundf((vy / MAP_VB_H) * (frameH - 12));
}

static bool pointInsideMapPoly(const MapPt* p, int n, float px, float py) {
  bool inside = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float xi = p[i].x, yi = p[i].y;
    float xj = p[j].x, yj = p[j].y;
    bool intersect = ((yi > py) != (yj > py)) &&
      (px < (xj - xi) * (py - yi) / ((yj - yi) == 0 ? 0.0001f : (yj - yi)) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

static void drawThickLine(int x1, int y1, int x2, int y2, uint16_t color, uint8_t thickness) {
  if (thickness <= 1) {
    display.drawLine(x1, y1, x2, y2, color);
    return;
  }

  // Simple 2px stroke: draw the original segment plus one neighboring
  // parallel pixel in the dominant normal direction. This keeps the flash
  // outline visibly heavier without expanding it into a fat blob.
  display.drawLine(x1, y1, x2, y2, color);
  int dx = abs(x2 - x1);
  int dy = abs(y2 - y1);
  if (dx >= dy) {
    display.drawLine(x1, y1 + 1, x2, y2 + 1, color);
  } else {
    display.drawLine(x1 + 1, y1, x2 + 1, y2, color);
  }
}

static void drawScaledMapPolyOutlineColor(const MapPt* p, int n, int frameX, int frameY, int frameW, int frameH, uint16_t color, uint8_t thickness = 1) {
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    drawThickLine(mapX(frameX, frameW, p[i].x), mapY(frameY, frameH, p[i].y),
                  mapX(frameX, frameW, p[j].x), mapY(frameY, frameH, p[j].y),
                  color, thickness);
  }
}

static void drawScaledMapPolyOutline(const MapPt* p, int n, int frameX, int frameY, int frameW, int frameH) {
  drawScaledMapPolyOutlineColor(p, n, frameX, frameY, frameW, frameH, GxEPD_BLACK, 1);
}

static void drawActiveZoneWhiteOutlines(int frameX, int frameY, int frameW, int frameH) {
  // Active zones are solid black fills. Their polygon borders stay stable and
  // white; the zone-switch cue is now shown by flashing the badge only.
  for (int zone = 0; zone < DISPLAY_ZONE_COUNT; zone++) {
    if (!state.activeZones[zone]) continue;
    const ZoneMapGroup& g = DISPLAY_ZONE_MAP[zone];
    for (uint8_t j = 0; j < g.count; j++) {
      uint8_t polyIndex = g.polyIndexes[j];
      if (polyIndex < MAP_POLY_COUNT) {
        drawScaledMapPolyOutlineColor(MAP_POLYS[polyIndex].p, MAP_POLYS[polyIndex].n,
                                      frameX, frameY, frameW, frameH, GxEPD_WHITE, 1);
      }
    }
  }
}

static void fillScaledMapPolyColor(const MapPt* p, int n, int frameX, int frameY, int frameW, int frameH, uint16_t color) {
  float minX = 9999, maxX = -9999, minY = 9999, maxY = -9999;
  for (int i = 0; i < n; i++) {
    minX = min(minX, p[i].x); maxX = max(maxX, p[i].x);
    minY = min(minY, p[i].y); maxY = max(maxY, p[i].y);
  }

  int sx0 = mapX(frameX, frameW, minX);
  int sx1 = mapX(frameX, frameW, maxX);
  int sy0 = mapY(frameY, frameH, minY);
  int sy1 = mapY(frameY, frameH, maxY);

  for (int sy = sy0; sy <= sy1; sy++) {
    bool inRun = false;
    int runStart = sx0;
    for (int sx = sx0; sx <= sx1; sx++) {
      float vx = ((float)(sx - frameX - 6) / (float)(frameW - 12)) * MAP_VB_W;
      float vy = ((float)(sy - frameY - 6) / (float)(frameH - 12)) * MAP_VB_H;
      bool inside = pointInsideMapPoly(p, n, vx, vy);
      if (inside && !inRun) { runStart = sx; inRun = true; }
      if ((!inside || sx >= sx1) && inRun) {
        int runEnd = inside ? sx : sx - 1;
        if (runEnd >= runStart) display.drawFastHLine(runStart, sy, runEnd - runStart + 1, color);
        inRun = false;
      }
    }
  }
}

static void fillScaledMapPoly(const MapPt* p, int n, int frameX, int frameY, int frameW, int frameH) {
  fillScaledMapPolyColor(p, n, frameX, frameY, frameW, frameH, GxEPD_BLACK);
}

static MapPt polygonCentroid(const MapPt* p, int n) {
  // Use the true polygon centroid, not the bounding-box center. If the SVG
  // polygon repeats its first point at the end, ignore that duplicate vertex.
  int m = n;
  if (m > 1 && fabsf(p[0].x - p[m - 1].x) < 0.001f && fabsf(p[0].y - p[m - 1].y) < 0.001f) m--;

  float twiceArea = 0.0f;
  float cx = 0.0f;
  float cy = 0.0f;
  for (int i = 0; i < m; i++) {
    int j = (i + 1) % m;
    float cross = p[i].x * p[j].y - p[j].x * p[i].y;
    twiceArea += cross;
    cx += (p[i].x + p[j].x) * cross;
    cy += (p[i].y + p[j].y) * cross;
  }

  if (fabsf(twiceArea) < 0.0001f) {
    // Degenerate fallback: average vertices. This is not expected for these
    // SVG zones, but prevents labels from going invalid if geometry changes.
    cx = 0.0f;
    cy = 0.0f;
    for (int i = 0; i < m; i++) { cx += p[i].x; cy += p[i].y; }
    return {cx / max(1, m), cy / max(1, m)};
  }

  float factor = 1.0f / (3.0f * twiceArea);
  return {cx * factor, cy * factor};
}

void drawZoneNumberBadge(int frameX, int frameY, int frameW, int frameH, int zone, const MapPt* p, int n, bool active) {
  MapPt c = polygonCentroid(p, n);
  int cx = mapX(frameX, frameW, c.x);
  int cy = mapY(frameY, frameH, c.y);
  const int r = 15;

  // Always use a filled badge, not an outlined/empty badge.
  // Inactive: black filled circle with white number.
  // Active: white filled circle with black number. During the selected-zone
  // switch cue, only this badge flashes back to black/white for 0.5 seconds.
  bool flashInvert = active && isRuntimeZoneFlashBadgeInverted((uint8_t)zone);
  if (active && !flashInvert) {
    display.fillCircle(cx, cy, r, GxEPD_WHITE);
    display.drawCircle(cx, cy, r, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
  } else {
    display.fillCircle(cx, cy, r, GxEPD_BLACK);
    display.drawCircle(cx, cy, r, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
  }

  display.setFont(&FreeMonoBold12pt7b);
  char label[4];
  snprintf(label, sizeof(label), "%d", zone);
  int16_t tx, ty;
  uint16_t tw, th;
  display.getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
  display.setCursor(cx - (int)tw / 2 - tx, cy - (int)th / 2 - ty);
  display.print(label);
  display.setTextColor(GxEPD_BLACK);
}

void drawMapLinework(int x, int y, int w, int h) {
  // Important boundary/linework from the Castle Hills SVG. Kept lightweight so
  // the e-paper map is recognizable without carrying a full SVG renderer.
  display.drawLine(mapX(x,w,10.000f), mapY(y,h,266.311f), mapX(x,w,251.526f), mapY(y,h,274.667f), GxEPD_BLACK);
  display.drawLine(mapX(x,w,282.082f), mapY(y,h,191.277f), mapX(x,w,276.705f), mapY(y,h,168.862f), GxEPD_BLACK);
  display.drawLine(mapX(x,w,276.705f), mapY(y,h,168.862f), mapX(x,w,277.246f), mapY(y,h,95.213f), GxEPD_BLACK);
  display.drawLine(mapX(x,w,277.246f), mapY(y,h,95.213f), mapX(x,w,277.700f), mapY(y,h,15.387f), GxEPD_BLACK);
  display.drawLine(mapX(x,w,13.295f), mapY(y,h,258.387f), mapX(x,w,35.211f), mapY(y,h,10.000f), GxEPD_BLACK);
  display.drawLine(mapX(x,w,35.211f), mapY(y,h,10.000f), mapX(x,w,37.872f), mapY(y,h,11.914f), GxEPD_BLACK);
  display.drawLine(mapX(x,w,37.872f), mapY(y,h,11.914f), mapX(x,w,276.960f), mapY(y,h,14.582f), GxEPD_BLACK);
  display.drawRect(mapX(x,w,251.266f), mapY(y,h,181.844f), 30, 15, GxEPD_BLACK);
  display.fillCircle(mapX(x,w,69.387f), mapY(y,h,54.012f), 2, GxEPD_BLACK);
  display.drawCircle(mapX(x,w,275.360f), mapY(y,h,167.849f), 4, GxEPD_BLACK);
  display.drawCircle(mapX(x,w,275.050f), mapY(y,h,97.811f), 4, GxEPD_BLACK);
  display.drawCircle(mapX(x,w,127.994f), mapY(y,h,239.046f), 4, GxEPD_BLACK);
}

void drawMap(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  drawMapLinework(x, y, w, h);

  for (int zone = 0; zone < DISPLAY_ZONE_COUNT; zone++) {
    if (!state.activeZones[zone]) continue;
    const ZoneMapGroup& g = DISPLAY_ZONE_MAP[zone];
    for (uint8_t j = 0; j < g.count; j++) {
      uint8_t polyIndex = g.polyIndexes[j];
      if (polyIndex < MAP_POLY_COUNT) {
        fillScaledMapPolyColor(MAP_POLYS[polyIndex].p, MAP_POLYS[polyIndex].n, x, y, w, h, GxEPD_BLACK);
      }
    }
  }

  for (int i = 0; i < MAP_POLY_COUNT; i++) {
    drawScaledMapPolyOutline(MAP_POLYS[i].p, MAP_POLYS[i].n, x, y, w, h);
  }

  drawActiveZoneWhiteOutlines(x, y, w, h);

  for (int zone = 0; zone < DISPLAY_ZONE_COUNT; zone++) {
    const ZoneMapGroup& g = DISPLAY_ZONE_MAP[zone];
    for (uint8_t j = 0; j < g.count; j++) {
      uint8_t polyIndex = g.polyIndexes[j];
      if (polyIndex < MAP_POLY_COUNT) {
        bool badgeActive = state.activeZones[zone];
        drawZoneNumberBadge(x, y, w, h, zone + 1, MAP_POLYS[polyIndex].p, MAP_POLYS[polyIndex].n, badgeActive);
      }
    }
  }
}

void partialUpdateMapPanel() {
  display.setPartialWindow(8, 48, 424, 424);
  display.firstPage();
  do {
    display.fillRect(8, 48, 424, 424, GxEPD_WHITE);
    drawMap(8, 48, 424, 424);
  } while (display.nextPage());
}

void partialUpdateRuntimePanel() {
  display.setPartialWindow(432, 373, 360, 99);
  display.firstPage();
  do {
    drawRuntimePanel(432, 373, 360, 99);
  } while (display.nextPage());
}

void drawWeatherIcon(int x, int y) {
  if (strstr(state.weather.condition, "Rain") || strstr(state.weather.condition, "rain")) {
    display.drawCircle(x, y, 16, GxEPD_BLACK);
    display.drawLine(x - 10, y + 22, x - 16, y + 32, GxEPD_BLACK);
    display.drawLine(x, y + 22, x - 6, y + 32, GxEPD_BLACK);
    display.drawLine(x + 10, y + 22, x + 4, y + 32, GxEPD_BLACK);
  } else if (strstr(state.weather.condition, "Cloud") || strstr(state.weather.condition, "cloud")) {
    display.drawCircle(x - 12, y, 12, GxEPD_BLACK);
    display.drawCircle(x + 2, y - 5, 16, GxEPD_BLACK);
    display.drawCircle(x + 18, y, 12, GxEPD_BLACK);
    display.drawLine(x - 24, y + 12, x + 30, y + 12, GxEPD_BLACK);
  } else {
    display.drawCircle(x, y, 18, GxEPD_BLACK);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4.0f;
      display.drawLine(x + cos(a) * 24, y + sin(a) * 24, x + cos(a) * 33, y + sin(a) * 33, GxEPD_BLACK);
    }
  }
}

void drawWindGauge(int cx, int cy, int r) {
  display.drawCircle(cx, cy, r, GxEPD_BLACK);
  display.setFont(&FreeMono9pt7b);
  display.setCursor(cx - 4, cy - r - 3); display.print("N");
  display.setCursor(cx + r + 3, cy + 4); display.print("E");
  display.setCursor(cx - 4, cy + r + 14); display.print("S");
  display.setCursor(cx - r - 13, cy + 4); display.print("W");

  float deg = state.weather.windDeg ? state.weather.windDeg : directionToDegrees(state.weather.windDirection);
  float a = (deg - 90.0f) * PI / 180.0f;
  int x2 = cx + cos(a) * (r - 1);
  int y2 = cy + sin(a) * (r - 1);
  display.drawLine(cx, cy, x2, y2, GxEPD_BLACK);
  display.fillCircle(x2, y2, 3, GxEPD_BLACK);
  display.setCursor(cx - 18, cy + 4);
  display.printf("%.0f", state.weather.windMph);
}

void drawWeatherWidget(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);

  if (!state.weatherLoaded) {
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(x + 20, y + 62);
    display.print("Unable to load");
    display.setCursor(x + 20, y + 92);
    display.print("weather data");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(x + 20, y + 124);
    display.print("Relay returned no weather.");
    return;
  }

  display.setFont(&FreeSans9pt7b);
  drawWeatherIcon(x + 35, y + 42);
  display.setFont(&FreeMonoBold12pt7b);
  display.setCursor(x + 70, y + 42);
  display.printf("%.0fF", state.weather.temperatureF);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 20, y + 82);
  display.print(state.weather.condition[0] ? state.weather.condition : "No weather");

  display.drawLine(x + 150, y + 8, x + 150, y + 118, GxEPD_BLACK);
  display.setCursor(x + 160, y + 24); display.printf("Humidity %.0f%%", state.weather.humidityPct);
  display.setCursor(x + 160, y + 44); display.printf("Dew point %.0fF", state.weather.dewPointF);
  display.setCursor(x + 160, y + 64); display.printf("Precip %.0f%%", state.weather.precipitationChancePct);
  display.setCursor(x + 160, y + 84); display.printf("Wind %s", state.weather.windDirection);
  display.setCursor(x + 160, y + 104); display.printf("Rain %.2fin", state.weather.rainIn);

  drawWindGauge(x + 314, y + 57, 31);

  display.drawLine(x + 8, y + 120, x + w - 8, y + 120, GxEPD_BLACK);
  char sunriseTxt[16], sunsetTxt[16];
  formatTimeLowerNoLeadingZero(state.weather.sunriseEpoch, sunriseTxt, sizeof(sunriseTxt));
  formatTimeLowerNoLeadingZero(state.weather.sunsetEpoch, sunsetTxt, sizeof(sunsetTxt));
  display.setCursor(x + 12, y + 137); display.print("Sunrise");
  display.setCursor(x + 12, y + 152); display.print(sunriseTxt);

  // Draw only a compact upper sun arc inside the weather panel. The previous
  // full-circle trick extended below y+160 and visually invaded the schedule panel.
  const int arcX0 = x + 104;
  const int arcX1 = x + 258;
  const int baseY = y + 149;
  const int arcH = 24;
  display.drawLine(arcX0, baseY, arcX1, baseY, GxEPD_BLACK);
  int prevX = arcX0;
  int prevY = baseY;
  for (int i = 1; i <= 24; i++) {
    float t = i / 24.0f;
    int px = arcX0 + (int)((arcX1 - arcX0) * t);
    int py = baseY - (int)(sinf(t * PI) * arcH);
    display.drawLine(prevX, prevY, px, py, GxEPD_BLACK);
    prevX = px;
    prevY = py;
  }

  unsigned long nowEpoch = time(nullptr);
  if (state.weather.sunriseEpoch > 0 && state.weather.sunsetEpoch > state.weather.sunriseEpoch) {
    float pct = (float)((long)nowEpoch - (long)state.weather.sunriseEpoch) / (float)((long)state.weather.sunsetEpoch - (long)state.weather.sunriseEpoch);
    pct = constrain(pct, 0.0f, 1.0f);
    int sx = arcX0 + (int)((arcX1 - arcX0) * pct);
    int sy = baseY - (int)(sinf(pct * PI) * arcH);
    display.fillCircle(sx, sy, 4, GxEPD_BLACK);
  }
  display.setCursor(x + 278, y + 137); display.print("Sunset");
  display.setCursor(x + 282, y + 152); display.print(sunsetTxt);
}

void drawSchedulePanel(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.setFont(&FreeMonoBold12pt7b);
  display.setCursor(x + 8, y + 22);
  display.print("Schedule");
  display.setFont(&FreeSans9pt7b);
  if (!state.scheduleLoaded) {
    display.setCursor(x + 10, y + 62);
    display.print("Not Loaded");
    display.setCursor(x + 10, y + 87);
    display.print("Retrying relay schedule...");
    return;
  }
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    // One schedule row per watering zone. Use wider spacing so the
    // schedule does not collapse into an unreadable text block.
    int sy = y + 50 + i * 25;

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x + 10, sy);
    display.printf("Zone %d", i + 1);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(x + 82, sy);
    if (!state.scheduleZoneLoaded[i]) {
      display.print("Not Loaded");
      continue;
    }

    ScheduleSlot& a = state.scheduleSlots[i][0];
    int hour = a.startHour % 12;
    if (hour == 0) hour = 12;
    display.printf("%d:%02d%s %um", hour, a.startMinute, a.startHour >= 12 ? "pm" : "am", a.runMinutes);

    ScheduleSlot& b = state.scheduleSlots[i][1];
    if (b.loaded) {
      int hour2 = b.startHour % 12;
      if (hour2 == 0) hour2 = 12;
      display.printf(" & %d:%02d%s %um", hour2, b.startMinute, b.startHour >= 12 ? "pm" : "am", b.runMinutes);
    }
  }
}

void drawRuntimePanel(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, GxEPD_WHITE);
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.setFont(&FreeMonoBold12pt7b);
  if (!state.run.active) {
    if (state.spigotsOn) {
      display.setFont(&FreeMonoBold12pt7b);
      display.setCursor(x + 8, y + 42);
      display.print("Spigots On");
      display.drawRect(x + 8, y + 58, w - 16, 26, GxEPD_BLACK);
      display.fillRect(x + 9, y + 59, w - 18, 24, GxEPD_BLACK);
    } else if (lastFinishedZone > 0) {
      display.setCursor(x + 8, y + 32); display.printf("Finished Zone %u", lastFinishedZone);
      display.drawRect(x + 8, y + 58, w - 16, 26, GxEPD_BLACK);
    } else {
      display.setCursor(x + 8, y + 32); display.print("Idle");
      display.drawRect(x + 8, y + 58, w - 16, 26, GxEPD_BLACK);
      display.setFont(&FreeSans9pt7b);
      display.setCursor(x + 8, y + h - 12);
      if (!relayDataReady && relayLastError[0]) display.print("Relay error");
      else display.print("Idle");
    }
    lastRuntimeMeterZone = 0;
    return;
  }

  uint8_t meterZone = runtimeMeterZone();
  if (meterZone < 1 || meterZone > DISPLAY_ZONE_COUNT) meterZone = state.run.zone;
  if (meterZone < 1 || meterZone > DISPLAY_ZONE_COUNT) meterZone = 1;
  uint16_t remaining = state.activeRemainingSeconds[meterZone - 1];
  uint16_t total = state.activeTotalSeconds[meterZone - 1];
  if (remaining == 0 && meterZone == state.run.zone) remaining = state.run.remainingSeconds;
  if (total == 0 && meterZone == state.run.zone) total = state.run.totalSeconds;

  // Keep the runtime panel compact so vertical space is available
  // for the schedule list. Do not add extra status text below;
  // the meter itself is the status. Zone-switch flashing is shown
  // on the actual map polygon, not in this header.
  display.setFont(&FreeMonoBold12pt7b);
  display.setCursor(x + 8, y + 42);
  display.printf("Running Zone %u", meterZone);

  // Keep the countdown visually secondary to the running-zone label.
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 238, y + 42);
  display.printf("%um %us", remaining / 60, remaining % 60);

  float r = total ? ((float)(total - remaining) / (float)total) : 0;
  r = constrain(r, 0.0f, 1.0f);
  display.drawRect(x + 8, y + 61, w - 16, 30, GxEPD_BLACK);
  display.fillRect(x + 9, y + 62, (int)((w - 18) * r), 28, GxEPD_BLACK);

  lastRuntimeMeterZone = meterZone;
}

void drawWrappedTextBlock(int x, int y, int maxWidth, int lineHeight, const char* text) {
  if (!text || !*text) return;
  int maxCharsPerLine = max(20, maxWidth / 10);
  char working[768];
  strlcpy(working, text, sizeof(working));
  char* token = strtok(working, " ");
  char line[160] = {0};
  int currentY = y;
  while (token && currentY < 460) {
    if (strlen(line) == 0) strlcpy(line, token, sizeof(line));
    else if ((int)(strlen(line) + 1 + strlen(token)) <= maxCharsPerLine) {
      strlcat(line, " ", sizeof(line));
      strlcat(line, token, sizeof(line));
    } else {
      display.setCursor(x, currentY);
      display.print(line);
      currentY += lineHeight;
      strlcpy(line, token, sizeof(line));
    }
    token = strtok(nullptr, " ");
  }
  if (strlen(line) > 0 && currentY < 460) {
    display.setCursor(x, currentY);
    display.print(line);
  }
}

void renderConnectionDiagnosticScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(8, 25);
    display.print("Garden E-Ink Relay Connection");
    display.drawLine(8, 48, 792, 48, GxEPD_BLACK);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(16, 78);
    display.print("The display is running, but relay data is not ready yet.");

    display.drawRect(8, 95, 784, 345, GxEPD_BLACK);
    display.setCursor(20, 125);
    display.printf("Admin AP: %s", apSsid);
    display.setCursor(20, 148);
    display.printf("Admin URL: http://%s", WiFi.softAPIP().toString().c_str());
    display.setCursor(20, 171);
    display.printf("Station SSID: %s", strlen(staSsid) ? staSsid : "NOT SET");
    display.setCursor(20, 194);
    display.printf("Station status: %s", wifiStatusName(WiFi.status()).c_str());
    display.setCursor(20, 217);
    display.printf("Station IP: %s", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "none");
    display.setCursor(20, 240);
    display.printf("Relay base: %s", relayBase);
    display.setCursor(20, 263);
    display.printf("Token: %s", strlen(relayApiToken) ? "configured" : "blank / not required");
    display.setCursor(20, 286);
    display.printf("Failure stage: %s", relayLastStage);

    display.drawLine(20, 300, 776, 300, GxEPD_BLACK);
    display.setCursor(20, 328);
    display.print("Last message:");
    drawWrappedTextBlock(20, 354, 744, 22, relayStatus);
  } while (display.nextPage());
}


void drawHeaderChurchCross(int x, int y) {
  // Small bold church-garden cross sized to the schedule title text height.
  // Drawn with filled rectangles so it stays crisp on e-paper.
  const int w = 14;
  const int h = 18;
  const int t = 5;
  const int cx = x + (w - t) / 2;
  display.fillRect(cx, y, t, h, GxEPD_BLACK);
  display.fillRect(x, y + 5, w, t, GxEPD_BLACK);
}

void renderScheduleScreenFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold12pt7b);
    drawHeaderChurchCross(10, 9);
    display.setCursor(34, 25);
    display.print("Castle Hills Garden Schedule");

    drawHeaderClockTextOnly();

    display.drawLine(8, 48, 792, 48, GxEPD_BLACK);
    drawMap(8, 48, 424, 424);
    drawWeatherWidget(432, 48, 360, 166);
    drawSchedulePanel(432, 213, 360, 161);
    drawRuntimePanel(432, 373, 360, 99);
  } while (display.nextPage());
}

void renderNewsScreenFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(8, 25);
    display.print("Castle Hills Garden News");
    display.setFont(&FreeSans9pt7b);
    display.drawLine(8, 48, 792, 48, GxEPD_BLACK);
    display.drawRect(8, 58, 784, 404, GxEPD_BLACK);
    display.setCursor(16, 82); display.print(state.date);
    display.setCursor(650, 82); display.print(state.time);
    display.drawLine(16, 92, 784, 92, GxEPD_BLACK);
    drawWrappedTextBlock(16, 120, 760, 20, state.gardenNews);
  } while (display.nextPage());
}

void drawGraphFrame(int x, int y, int w, int h, const char* title, const char* yTop, const char* yMid, const char* yBot) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 8, y + 16); display.print(title);
  display.drawLine(x + 72, y + 20, x + 72, y + h - 14, GxEPD_BLACK);
  display.drawLine(x + 72, y + h - 14, x + w - 12, y + h - 14, GxEPD_BLACK);
  display.setCursor(x + 8, y + 34); display.print(yTop);
  display.setCursor(x + 8, y + h / 2); display.print(yMid);
  display.setCursor(x + 8, y + h - 16); display.print(yBot);
}

int loadFakeHistory(HistoryRow* rows, int maxRows) {
  if (!rows || maxRows <= 0) return 0;
  char csv[768];
  strlcpy(csv, HISTORY_CSV_DATA, sizeof(csv));
  int count = 0;
  char* line = strtok(csv, "\n");
  bool skipHeader = true;
  while (line && count < maxRows) {
    if (!skipHeader) {
      HistoryRow r = {};
      char reason[24] = {0};
      if (sscanf(line, "%lu,%f,%f,%f,%f,%d,%23s", &r.epoch, &r.tempF, &r.rainIn, &r.sunlightHours, &r.windMph, &r.weatherCode, reason) == 7) {
        strlcpy(r.reason, reason, sizeof(r.reason));
        rows[count++] = r;
      }
    }
    skipHeader = false;
    line = strtok(nullptr, "\n");
  }
  return count;
}

void drawHistorySeriesLine(const HistoryRow* rows, int count, int x, int y, int w, int h, bool tempSeries) {
  if (!rows || count < 2) return;
  float minV = 9999.0f, maxV = -9999.0f;
  for (int i = 0; i < count; i++) {
    float v = tempSeries ? rows[i].tempF : rows[i].sunlightHours;
    minV = min(minV, v); maxV = max(maxV, v);
  }
  float span = max(0.1f, maxV - minV);
  for (int i = 1; i < count; i++) {
    int px0 = x + (i - 1) * (w - 1) / (count - 1);
    int px1 = x + i * (w - 1) / (count - 1);
    float v0 = tempSeries ? rows[i - 1].tempF : rows[i - 1].sunlightHours;
    float v1 = tempSeries ? rows[i].tempF : rows[i].sunlightHours;
    int py0 = y + h - 1 - (int)(((v0 - minV) / span) * (h - 1));
    int py1 = y + h - 1 - (int)(((v1 - minV) / span) * (h - 1));
    display.drawLine(px0, py0, px1, py1, GxEPD_BLACK);
  }
}

void drawHistorySeriesRainBars(const HistoryRow* rows, int count, int x, int y, int w, int h) {
  if (!rows || count <= 0) return;
  float maxRain = 0.0f;
  for (int i = 0; i < count; i++) maxRain = max(maxRain, rows[i].rainIn);
  maxRain = max(0.1f, maxRain);
  int barW = max(3, (w - 2) / max(1, count));
  for (int i = 0; i < count; i++) {
    int px = x + 1 + i * (w - 2) / max(1, count);
    int bh = (int)((rows[i].rainIn / maxRain) * (h - 2));
    display.fillRect(px, y + h - 1 - bh, barW - 1, bh, GxEPD_BLACK);
  }
}

void renderGraphScreenFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(8, 25); display.print("Current + Weekly Weather");
    display.setFont(&FreeSans9pt7b);
    display.drawLine(8, 48, 792, 48, GxEPD_BLACK);
    display.setCursor(16, 72); display.print(state.date);
    display.setCursor(680, 72); display.print(state.time);
    display.drawRect(8, 84, 784, 84, GxEPD_BLACK);
    display.setCursor(16, 106); display.printf("Now %.0fF %s", state.weather.temperatureF, state.weather.condition);
    display.setCursor(16, 126); display.printf("Rain %.2fin Sun %.1fhr Wind %.0fmph", state.weather.rainIn, state.weather.sunlightHours, state.weather.windMph);
    display.drawRect(8, 176, 784, 50, GxEPD_BLACK);
    display.setCursor(16, 202); display.print("7-day forecast strip");
    display.drawRect(8, 230, 784, 50, GxEPD_BLACK);
    display.setCursor(16, 256); display.print("8-slot hourly forecast strip");
    drawGraphFrame(8, 285, 784, 60, "Temp F", "90", "70", "50");
    drawGraphFrame(8, 350, 784, 60, "Rain in", "1.0", "0.5", "0.0");
    drawGraphFrame(8, 415, 784, 50, "Sun hrs", "12", "6", "0");
    HistoryRow rows[8];
    int rowCount = loadFakeHistory(rows, 8);
    drawHistorySeriesLine(rows, rowCount, 84, 306, 696, 24, true);
    drawHistorySeriesRainBars(rows, rowCount, 84, 371, 696, 24);
    drawHistorySeriesLine(rows, rowCount, 84, 431, 696, 18, false);
  } while (display.nextPage());
}

void drawScreen() {
  if (!relayDataReady && forceDiagnosticScreen) {
    renderConnectionDiagnosticScreen();
    return;
  }

  applyScreenRotationMode();
  if (strstr(state.title, "Garden News")) renderNewsScreenFull();
  else if (strstr(state.title, "Weekly Weather")) renderGraphScreenFull();
  else renderScheduleScreenFull();
}

bool substantialChange() {
  if (forceFullRedraw) return true;
  if (state.run.active != lastDrawn.run.active) return true;
  if (state.run.zone != lastDrawn.run.zone) return true;
  if (strcmp(state.title, lastDrawn.title) != 0) return true;
  if (state.scheduleLoaded != lastDrawn.scheduleLoaded) return true;
  if (state.spigotsOn != lastDrawn.spigotsOn) return true;
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    if (state.scheduleZoneLoaded[i] != lastDrawn.scheduleZoneLoaded[i]) return true;
    if (state.activeZones[i] != lastDrawn.activeZones[i]) return true;
    if (state.activeTotalSeconds[i] != lastDrawn.activeTotalSeconds[i]) return true;
    for (int slot = 0; slot < 2; slot++) {
      if (state.scheduleSlots[i][slot].loaded != lastDrawn.scheduleSlots[i][slot].loaded) return true;
      if (state.scheduleSlots[i][slot].startHour != lastDrawn.scheduleSlots[i][slot].startHour) return true;
      if (state.scheduleSlots[i][slot].startMinute != lastDrawn.scheduleSlots[i][slot].startMinute) return true;
      if (state.scheduleSlots[i][slot].runMinutes != lastDrawn.scheduleSlots[i][slot].runMinutes) return true;
    }
  }
  static bool lastRelayReadySeen = false;
  static bool lastDiagnosticSeen = true;
  if (relayDataReady != lastRelayReadySeen || forceDiagnosticScreen != lastDiagnosticSeen) {
    lastRelayReadySeen = relayDataReady;
    lastDiagnosticSeen = forceDiagnosticScreen;
    return true;
  }
  return false;
}


String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

String htmlInput(const char* id, const char* label, const String& value, const char* type = "text", const char* extra = "") {
  String out;
  out += F("<label>");
  out += label;
  out += F("</label><input id='");
  out += id;
  out += F("' name='");
  out += id;
  out += F("' type='");
  out += type;
  out += F("' value='");
  out += htmlEscape(value);
  out += F("' ");
  out += extra;
  out += F(">");
  return out;
}


String portalUrl() {
  return String("http://") + WiFi.softAPIP().toString() + "/";
}

void redirectToPortal() {
  server.sendHeader("Location", portalUrl(), true);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(302, "text/plain", "Redirecting to Garden E-Ink Admin");
}

void handleCaptivePortalProbe() {
  redirectToPortal();
}

void handleNotFound() {
  // DNS redirects all hostnames to the e-ink AP. Any unknown HTTP path should
  // land on the admin page instead of showing a 404 or an OS probe response.
  redirectToPortal();
}

void handleRoot() {
  String page;
  page.reserve(11000);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Garden E-Ink Admin</title><style>");
  page += F("body{font-family:Arial,sans-serif;margin:0;background:#f4f1e8;color:#111}header{background:#111;color:#fff;padding:14px 16px}h1{font-size:20px;margin:0}main{padding:12px;max-width:900px;margin:auto}section{background:#fff;border:1px solid #222;margin:12px 0;padding:12px;box-shadow:2px 2px 0 #222}h2{font-size:17px;margin:0 0 10px}label{display:block;margin:8px 0 3px;font-weight:700}input,textarea,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #333;background:#fff;font-size:15px}button,.btn{display:inline-block;margin:6px 6px 6px 0;padding:10px 12px;border:1px solid #111;background:#111;color:#fff;font-weight:700;text-decoration:none;cursor:pointer}.secondary{background:#fff;color:#111}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.status{white-space:pre-wrap;border:1px solid #333;padding:10px;background:#fafafa}.small{font-size:13px;color:#333}.ok{font-weight:700}.bad{font-weight:700;color:#8a0000}@media(max-width:650px){.grid{grid-template-columns:1fr}}");
  page += F("</style></head><body><header><h1>Garden E-Ink Admin</h1></header><main>");

  page += F("<section><h2>Status</h2><div class='status'>");
  page += relayDataReady ? F("Relay data ready\n") : F("Relay data not ready\n");
  page += F("Title: "); page += htmlEscape(state.title); page += F("\n");
  page += F("Date/time: "); page += htmlEscape(state.date); page += F(" "); page += htmlEscape(state.time); page += F("\n");
  page += F("AP: "); page += htmlEscape(apSsid); page += F(" "); page += WiFi.softAPIP().toString(); page += F("\n");
  page += F("STA: "); page += htmlEscape(staSsid); page += F(" "); page += wifiStatusName(WiFi.status()); page += F(" "); page += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("none")); page += F("\n");
  page += F("Relay: "); page += htmlEscape(relayBase); page += F("\n");
  page += F("Stage: "); page += htmlEscape(relayLastStage); page += F("\n");
  page += F("Message: "); page += htmlEscape(relayStatus);
  page += F("</div><button type='button' data-url='/sync'>Sync Relay</button><button type='button' data-url='/redraw'>Redraw E-Paper</button><button type='button' data-url='/stop'>Stop / All Off</button><pre id='ajaxResult' class='status'>Ready.</pre></section>");

  page += F("<section><h2>Wi-Fi + Relay Token Settings</h2><p class='small'>Saving these settings reboots the display so Wi-Fi can restart cleanly. Reconnect to the admin AP after it comes back up.</p>");
  page += F("<form method='get' action='/saveConnectivity'><div class='grid'><div>");
  page += htmlInput("apSsid", "Admin AP SSID", apSsid, "text", "maxlength='31'");
  page += htmlInput("apPass", "Admin AP Password", "", "password", "maxlength='63' placeholder='leave blank to keep existing; 8+ chars to change'");
  page += F("</div><div>");
  page += htmlInput("staSsid", "Home / Station Wi-Fi SSID", staSsid, "text", "maxlength='31'");
  page += htmlInput("staPass", "Home / Station Wi-Fi Password", "", "password", "maxlength='63' placeholder='leave blank to keep existing'");
  page += F("</div></div>");
  page += htmlInput("relayBase", "Relay Base URL", relayBase, "text", "maxlength='127' placeholder='http://192.168.4.1'");
  page += htmlInput("relayApiToken", "Relay API Token", "", "password", "maxlength='95' placeholder='leave blank to keep existing / blank if unused'");
  page += F("<button type='submit'>Save Wi-Fi / Relay Settings</button></form></section>");

  page += F("<section><h2>Display Mode</h2><p><b>Schedule screen only is temporarily enabled.</b> News and weather rotation are disabled for field testing.</p><button type='button' data-url='/display?mode=schedule'>Show Schedule</button></section>");

  page += F("<section><h2>Manual Extra Water</h2><form class='ajaxGet' method='get' action='/extra'><label>Zone</label><select name='zone'><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select><label>Minutes</label><input name='minutes' type='number' value='10' min='1' max='240'><button type='submit'>Queue Extra Water</button></form></section>");

  page += F("<section><h2>Zones</h2><form class='ajaxGet' method='get' action='/saveZone'><div class='grid'><div><label>Zone</label><select name='zone'><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select>");
  page += htmlInput("name", "Name", state.zones[0].name, "text", "maxlength='23'");
  page += htmlInput("baseMinutes", "Base Minutes", String(state.zones[0].baseMinutes), "number", "min='1' max='240'");
  page += F("</div><div>");
  page += htmlInput("startHour", "Start Hour 0-23", String(state.zones[0].startHour), "number", "min='0' max='23'");
  page += htmlInput("startMinute", "Start Minute 0-59", String(state.zones[0].startMinute), "number", "min='0' max='59'");
  page += F("<button type='submit'>Save Zone</button><p class='small'>This no-JavaScript form preloads Zone 1 values. Select another zone and enter its values manually.</p></div></div></form></section>");

  page += F("<section><h2>Full-Screen Garden News</h2><form class='ajaxGet' method='get' action='/saveNewsForm'><textarea name='news' rows='5'>");
  page += htmlEscape(state.gardenNews);
  page += F("</textarea><button type='submit'>Save News to Display</button></form></section>");

  page += F("<section><h2>Weather History</h2><a class='btn' href='/history.csv'>Download CSV</a><button type='button' class='secondary' data-url='/clearHistory'>Clear History</button></section>");
  page += F("<script>function setResult(t){var e=document.getElementById('ajaxResult');if(e){e.textContent=t;}}function apiCall(u){setResult('Sending '+u+' ...');fetch(u,{cache:'no-store'}).then(function(r){return r.text().then(function(t){setResult('HTTP '+r.status+' '+u+'\n'+t);});}).catch(function(e){setResult('ERROR '+u+'\n'+e);});}document.addEventListener('DOMContentLoaded',function(){document.querySelectorAll('[data-url]').forEach(function(b){b.addEventListener('click',function(){apiCall(b.getAttribute('data-url'));});});document.querySelectorAll('form.ajaxGet').forEach(function(f){f.addEventListener('submit',function(ev){ev.preventDefault();var qs=new URLSearchParams(new FormData(f)).toString();apiCall(f.getAttribute('action')+(qs?'?'+qs:''));});});});</script>");
  page += F("</main></body></html>");
  server.send(200, "text/html", page);
}

void handleSaveConnectivity() {
  if (server.hasArg("apSsid")) {
    String v = server.arg("apSsid");
    if (v.length() < 1 || v.length() >= sizeof(apSsid)) {
      server.send(400, "text/plain", "apSsid must be 1-31 characters");
      return;
    }
    strlcpy(apSsid, v.c_str(), sizeof(apSsid));
  }
  if (server.hasArg("apPass") && server.arg("apPass").length() > 0) {
    String v = server.arg("apPass");
    if (v.length() < 8 || v.length() >= sizeof(apPass)) {
      server.send(400, "text/plain", "AP password must be blank/unchanged or 8-63 characters");
      return;
    }
    strlcpy(apPass, v.c_str(), sizeof(apPass));
  }
  if (server.hasArg("staSsid")) strlcpy(staSsid, server.arg("staSsid").c_str(), sizeof(staSsid));
  if (server.hasArg("staPass") && server.arg("staPass").length() > 0) strlcpy(staPass, server.arg("staPass").c_str(), sizeof(staPass));
  if (server.hasArg("relayBase")) strlcpy(relayBase, server.arg("relayBase").c_str(), sizeof(relayBase));
  if (server.hasArg("relayApiToken") && server.arg("relayApiToken").length() > 0) strlcpy(relayApiToken, server.arg("relayApiToken").c_str(), sizeof(relayApiToken));

  saveConfig();
  server.send(200, "text/html", "<!doctype html><html><body><h1>Connectivity saved</h1><p>The display will reboot now so Wi-Fi can restart cleanly. Reconnect to the admin AP after it comes back up.</p></body></html>");
  delay(750);
  ESP.restart();
}

void handleSaveNewsForm() {
  if (server.hasArg("news")) {
    strlcpy(state.gardenNews, server.arg("news").c_str(), sizeof(state.gardenNews));
    saveConfig();
    forceFullRedraw = true;
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "saved");
}

void handleState() {
  StaticJsonDocument<8192> doc;
  doc["title"] = state.title;
  doc["date"] = state.date;
  doc["time"] = state.time;
  doc["masterEnable"] = state.masterEnable;
  doc["spigotsOn"] = state.spigotsOn;
  doc["weatherAdjustmentEnabled"] = state.weatherAdjustmentEnabled;
  doc["gardenNews"] = state.gardenNews;
  doc["currentRunActive"] = state.run.active;
  doc["currentRunZone"] = state.run.zone;
  doc["scheduleLoaded"] = state.scheduleLoaded;
  doc["displayMode"] = state.displayMode;
  doc["apSsid"] = apSsid;
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["staSsid"] = staSsid;
  doc["staIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("none");
  doc["staStatus"] = wifiStatusName(WiFi.status());
  doc["relayBase"] = relayBase;
  doc["relayStage"] = relayLastStage;
  doc["relayStatus"] = relayStatus;
  doc["relayLastError"] = relayLastError;
  doc["relayDataReady"] = relayDataReady;
  doc["tokenConfigured"] = strlen(relayApiToken) > 0;

  JsonObject weather = doc.createNestedObject("weather");
  weather["loaded"] = state.weatherLoaded;
  weather["summary"] = state.weather.summary;
  weather["condition"] = state.weather.condition;
  weather["temperatureF"] = state.weather.temperatureF;
  weather["humidityPct"] = state.weather.humidityPct;
  weather["dewPointF"] = state.weather.dewPointF;
  weather["precipitationChancePct"] = state.weather.precipitationChancePct;
  weather["windMph"] = state.weather.windMph;
  weather["windDeg"] = state.weather.windDeg;
  weather["windDirection"] = state.weather.windDirection;
  weather["rainIn"] = state.weather.rainIn;
  weather["sunlightHours"] = state.weather.sunlightHours;
  weather["sunriseEpoch"] = state.weather.sunriseEpoch;
  weather["sunsetEpoch"] = state.weather.sunsetEpoch;

  doc["queueState"] = queueStopped ? "stopped" : "running";
  doc["queueDepth"] = queueDepth;
  doc["pendingExtraZone"] = pendingExtraZone;
  doc["pendingExtraMinutes"] = pendingExtraMinutes;

  JsonArray activeZonesJson = doc.createNestedArray("activeZones");
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) activeZonesJson.add(state.activeZones[i]);

  JsonArray zones = doc.createNestedArray("zones");
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    JsonObject z = zones.createNestedObject();
    z["name"] = state.zones[i].name;
    z["baseMinutes"] = state.zones[i].baseMinutes;
    z["startHour"] = state.zones[i].startHour;
    z["startMinute"] = state.zones[i].startMinute;
    z["scheduleLoaded"] = state.scheduleZoneLoaded[i];
  }

  JsonArray ledger = doc.createNestedArray("soilLedger");
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) ledger.add(zoneLedger[i]);

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleConfigGet() {
  StaticJsonDocument<768> d;
  d["apSsid"] = apSsid;
  d["apPassConfigured"] = strlen(apPass) > 0;
  d["staSsid"] = staSsid;
  d["staPassConfigured"] = strlen(staPass) > 0;
  d["relayBase"] = relayBase;
  d["relayApiTokenConfigured"] = strlen(relayApiToken) > 0;
  d["apIp"] = WiFi.softAPIP().toString();
  d["staIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("none");
  d["staStatus"] = wifiStatusName(WiFi.status());
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleConfigPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing JSON body\"}");
    return;
  }

  DynamicJsonDocument d(1536);
  DeserializationError err = deserializeJson(d, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", String("{\"ok\":false,\"error\":\"JSON parse failed: ") + err.c_str() + "\"}");
    return;
  }

  if (d["apSsid"].is<const char*>()) {
    const char* v = d["apSsid"];
    if (strlen(v) < 1 || strlen(v) >= sizeof(apSsid)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"apSsid must be 1-31 characters\"}");
      return;
    }
    strlcpy(apSsid, v, sizeof(apSsid));
  }

  if (d["apPass"].is<const char*>()) {
    const char* v = d["apPass"];
    if (strlen(v) > 0 && strlen(v) < 8) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"AP password must be blank or at least 8 characters\"}");
      return;
    }
    strlcpy(apPass, v, sizeof(apPass));
  }

  if (d["staSsid"].is<const char*>()) strlcpy(staSsid, d["staSsid"], sizeof(staSsid));
  if (d["staPass"].is<const char*>()) strlcpy(staPass, d["staPass"], sizeof(staPass));
  if (d["relayBase"].is<const char*>()) strlcpy(relayBase, d["relayBase"], sizeof(relayBase));
  if (d["relayApiToken"].is<const char*>()) strlcpy(relayApiToken, d["relayApiToken"], sizeof(relayApiToken));

  saveConfig();
  setupWifi();
  forceFullRedraw = true;
  relayDataReady = false;
  forceDiagnosticScreen = true;
  syncFromRelay();

  StaticJsonDocument<512> out;
  out["ok"] = true;
  out["message"] = "Config saved. If the AP SSID/password changed, reconnect to the admin AP.";
  out["apSsid"] = apSsid;
  out["apIp"] = WiFi.softAPIP().toString();
  out["staStatus"] = wifiStatusName(WiFi.status());
  out["relayDataReady"] = relayDataReady;
  String json;
  serializeJson(out, json);
  server.send(200, "application/json", json);
}

void handleDisplayMode() {
  String m = server.arg("mode");
  m.toLowerCase();

  if (SCHEDULE_SCREEN_ONLY) {
    m = MODE_SCHEDULE;
  }

  // The admin UI/user language may call the graph screen "weather".
  // Internally the firmware uses MODE_GRAPH for the full-screen weather/history page.
  if (m == "weather" || m == "history" || m == "historic-weather") {
    m = MODE_GRAPH;
  }

  if (!(m == MODE_AUTO || m == MODE_SCHEDULE || m == MODE_NEWS || m == MODE_GRAPH)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"mode must be auto, schedule, news, graph, or weather\"}");
    return;
  }

  strlcpy(state.displayMode, m.c_str(), sizeof(state.displayMode));
  saveConfig();

  // Reset the auto cycle anchor when the user explicitly changes modes so
  // resume-auto starts a fresh rotation instead of landing mid-cycle.
  if (m == MODE_AUTO) {
    rotationEpochMs = millis();
  }

  // Do not wait for the next relay poll. The button press is a display command
  // and should redraw immediately.
  forceDiagnosticScreen = !relayDataReady;
  forceFullRedraw = true;
  drawScreen();
  lastDrawn = state;
  forceFullRedraw = false;

  String out = String("{\"ok\":true,\"mode\":") + jsonString(m) + ",\"title\":" + jsonString(state.title) + "}";
  server.send(200, "application/json", out);
}

void handleRedraw() {
  forceFullRedraw = true;
  drawScreen();
  lastDrawn = state;
  forceFullRedraw = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSync() {
  bool ok = syncFromRelay();
  forceFullRedraw = true;
  drawScreen();
  lastDrawn = state;
  forceFullRedraw = false;
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Relay sync failed; see /state for relayStatus\"}");
}


String jsonString(const String& value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if ((uint8_t)c < 32) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}

void handleExtra() {
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (zone < 1 || zone > DISPLAY_ZONE_COUNT || minutes < 1 || minutes > 240) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"zone must be 1-5 and minutes 1-240\"}");
    return;
  }

  String relayBody;
  String relayPath = String("/api/manual-run?zone=") + zone + "&minutes=" + minutes;
  bool relayOk = relayGetText(relayPath, "Relay manual run /api/manual-run", relayBody);

  if (relayOk) {
    queueStopped = false;
    if (queueDepth < 250) queueDepth++;
    pendingExtraZone = zone;
    pendingExtraMinutes = minutes;
    syncFromRelay();
    forceFullRedraw = true;
    server.send(200, "application/json", String("{\"ok\":true,\"relayForwarded\":true,\"relayBody\":") + jsonString(relayBody) + "}");
  } else {
    forceFullRedraw = true;
    drawScreen();
    server.send(502, "application/json", String("{\"ok\":false,\"error\":") + jsonString(relayStatus) + "}");
  }
}

void handleStop() {
  String relayBody;
  bool relayOk = relayGetText("/api/alloff", "Relay all off /api/alloff", relayBody);

  queueStopped = true;
  queueDepth = 0;
  state.run.active = false;
  state.run.zone = 0;
  state.run.remainingSeconds = 0;
  state.run.totalSeconds = 0;
  pendingExtraZone = 0;
  pendingExtraMinutes = 0;
  forceFullRedraw = true;

  if (relayOk) {
    syncFromRelay();
    server.send(200, "application/json", String("{\"ok\":true,\"stopped\":true,\"relayForwarded\":true,\"relayBody\":") + jsonString(relayBody) + "}");
  } else {
    drawScreen();
    server.send(502, "application/json", String("{\"ok\":false,\"stoppedLocal\":true,\"error\":") + jsonString(relayStatus) + "}");
  }
}

void handleSaveZone() {
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  if (zone < 1 || zone > DISPLAY_ZONE_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"zone must be 1-5\"}");
    return;
  }
  ZoneCfg& z = state.zones[zone - 1];
  if (server.hasArg("name")) strlcpy(z.name, server.arg("name").c_str(), sizeof(z.name));
  if (server.hasArg("baseMinutes")) z.baseMinutes = constrain(server.arg("baseMinutes").toInt(), 1, 240);
  if (server.hasArg("startHour")) z.startHour = constrain(server.arg("startHour").toInt(), 0, 23);
  if (server.hasArg("startMinute")) z.startMinute = constrain(server.arg("startMinute").toInt(), 0, 59);
  saveConfig();
  forceFullRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSaveLogic() {
  forceFullRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSaveNews() {
  if (server.hasArg("plain")) {
    strlcpy(state.gardenNews, server.arg("plain").c_str(), sizeof(state.gardenNews));
    saveConfig();
    forceFullRedraw = true;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleHistoryCsv() { server.send(200, "text/csv", HISTORY_CSV_DATA); }
void handleClearHistory() { server.send(200, "application/json", "{\"ok\":true}"); }
void handleQueueClear() { queueDepth = 0; server.send(200, "application/json", "{\"ok\":true,\"queueDepth\":0}"); }
void handleQueueStopClear() { queueStopped = true; queueDepth = 0; server.send(200, "application/json", "{\"ok\":true,\"queueState\":\"stopped\"}"); }
void handleLedgerReset() { for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) zoneLedger[i] = 0; server.send(200, "application/json", "{\"ok\":true}"); }

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/saveConnectivity", HTTP_GET, handleSaveConnectivity);
  server.on("/saveNewsForm", HTTP_GET, handleSaveNewsForm);
  server.on("/extra", HTTP_GET, handleExtra);
  server.on("/api/manual-run", HTTP_GET, handleExtra);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/api/alloff", HTTP_GET, handleStop);
  server.on("/sync", HTTP_GET, handleSync);
  server.on("/redraw", HTTP_GET, handleRedraw);
  server.on("/saveZone", HTTP_GET, handleSaveZone);
  server.on("/saveLogic", HTTP_GET, handleSaveLogic);
  server.on("/saveNews", HTTP_POST, handleSaveNews);
  server.on("/history.csv", HTTP_GET, handleHistoryCsv);
  server.on("/clearHistory", HTTP_GET, handleClearHistory);
  server.on("/display", HTTP_GET, handleDisplayMode);
  server.on("/queue/clear", HTTP_GET, handleQueueClear);
  server.on("/queue/stop-clear", HTTP_GET, handleQueueStopClear);
  server.on("/ledger/reset", HTTP_GET, handleLedgerReset);

  // Captive portal detection endpoints used by common clients. Redirecting
  // these makes phones/laptops open the Garden E-Ink Admin page after joining
  // the AP instead of showing a blank/success probe page.
  server.on("/generate_204", HTTP_GET, handleCaptivePortalProbe);
  server.on("/gen_204", HTTP_GET, handleCaptivePortalProbe);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalProbe);
  server.on("/library/test/success.html", HTTP_GET, handleCaptivePortalProbe);
  server.on("/connecttest.txt", HTTP_GET, handleCaptivePortalProbe);
  server.on("/ncsi.txt", HTTP_GET, handleCaptivePortalProbe);
  server.on("/fwlink", HTTP_GET, handleCaptivePortalProbe);
  server.on("/success.txt", HTTP_GET, handleCaptivePortalProbe);
  server.onNotFound(handleNotFound);

  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Garden E-Ink Zone Display booting...");

  // Shared SPI bus using the verified target-board pinout.
  SPI.begin(EPD_SCLK_PIN, SD_MISO_PIN, EPD_MOSI_PIN, EPD_CS_PIN);

  loadConfig();
  setupWifi();
  setupRoutes();

  display.init(115200);
  display.setRotation(0);
  rotationEpochMs = millis();

  setRelayStatus("Boot", "Display initialized. Attempting relay sync.", false);
  drawScreen();

  syncFromRelay();
  forceFullRedraw = true;
  drawScreen();
  lastDrawn = state;
  forceFullRedraw = false;
}

void loop() {
  dns.processNextRequest();
  server.handleClient();

  unsigned long nowMs = millis();
  if (relayDataReady && updateHeaderClockFromSystem()) {
    if (strstr(state.title, "Garden Schedule")) {
      partialUpdateHeaderClock();
      strlcpy(lastDrawn.date, state.date, sizeof(lastDrawn.date));
      strlcpy(lastDrawn.time, state.time, sizeof(lastDrawn.time));
    }
  }

  bool didPoll = false;
  bool statePollOk = false;

  if (!relayDataReady && nowMs - lastFullSyncMs >= RELAY_RETRY_SYNC_MS) {
    lastFullSyncMs = nowMs;
    statePollOk = syncFromRelay();
    didPoll = true;
  } else if (relayDataReady && nowMs - lastFullSyncMs >= RELAY_FULL_SYNC_MS) {
    lastFullSyncMs = nowMs;
    statePollOk = syncFromRelay();
    didPoll = true;
  } else if (nowMs - lastPollMs >= RELAY_STATE_POLL_MS) {
    statePollOk = syncRunStateOnly();
    didPoll = true;
  }

  uint8_t currentRuntimeMeterZone = state.run.active ? runtimeMeterZone() : 0;
  bool runtimeZoneChanged = state.run.active && currentRuntimeMeterZone > 0 && currentRuntimeMeterZone != lastRuntimeMeterZone;
  if (runtimeZoneChanged) {
    runtimeSwitchFlashZone = currentRuntimeMeterZone;
    runtimeSwitchFlashUntilMs = nowMs + RUNTIME_ZONE_FLASH_MS;
    runtimeSwitchFlashVisible = true;
    runtimeSwitchFlashLastPhase = 255;
  }

  bool runtimeFlashExpired = runtimeSwitchFlashVisible && (int32_t)(nowMs - runtimeSwitchFlashUntilMs) >= 0;
  bool runtimeFlashPhaseChanged = false;
  if (runtimeFlashExpired) {
    runtimeSwitchFlashVisible = false;
    runtimeSwitchFlashLastPhase = 255;
  } else if (runtimeSwitchFlashVisible) {
    uint8_t phase = runtimeZoneFlashPhase();
    if (phase != runtimeSwitchFlashLastPhase) {
      runtimeFlashPhaseChanged = true;
      runtimeSwitchFlashLastPhase = phase;
    }
  }

  if (!didPoll && !runtimeZoneChanged && !runtimeFlashExpired && !runtimeFlashPhaseChanged) return;
  if (didPoll) lastPollMs = nowMs;

  static bool previousRunActive = false;
  static uint8_t previousRunZone = 0;
  if (previousRunActive && !state.run.active && previousRunZone > 0) lastFinishedZone = previousRunZone;
  if (state.run.active && state.run.zone > 0) {
    previousRunZone = state.run.zone;
    lastFinishedZone = 0;
  }
  previousRunActive = state.run.active;

  if (state.run.active && state.run.zone >= 1 && state.run.zone <= DISPLAY_ZONE_COUNT) {
    zoneLedger[state.run.zone - 1] += 0.25f;
  }

  bool needFull = substantialChange();
  if (needFull) {
    drawScreen();
    lastDrawn = state;
    forceFullRedraw = false;
  } else if (relayDataReady && state.run.active && (runtimeZoneChanged || runtimeFlashExpired || runtimeFlashPhaseChanged || state.run.remainingSeconds != lastDrawn.run.remainingSeconds || memcmp(state.activeRemainingSeconds, lastDrawn.activeRemainingSeconds, sizeof(state.activeRemainingSeconds)) != 0)) {
    // Update the meter/header first so the newly selected zone is visible,
    // then blink only that zone's badge as the cue.
    partialUpdateRuntimePanel();
    if (runtimeZoneChanged || runtimeFlashExpired || runtimeFlashPhaseChanged) {
      partialUpdateMapPanel();
    }
    lastDrawn.run.remainingSeconds = state.run.remainingSeconds;
    lastDrawn.run.zone = state.run.zone;
    lastDrawn.run.active = state.run.active;
    lastDrawn.run.totalSeconds = state.run.totalSeconds;
    lastDrawn.spigotsOn = state.spigotsOn;
    memcpy(lastDrawn.activeRemainingSeconds, state.activeRemainingSeconds, sizeof(state.activeRemainingSeconds));
    memcpy(lastDrawn.activeTotalSeconds, state.activeTotalSeconds, sizeof(state.activeTotalSeconds));
  }
}
