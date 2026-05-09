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
#include <math.h>
#include <time.h>
#include <sys/time.h>

// Required hardware target: 7.5-inch 800x480 GDEY075T7 black/white panel.
// Verified sample pinout: EPD MOSI=23, SCLK=18, CS=27, DC=14, RST=33, BUSY=13.
static const uint8_t EPD_MOSI_PIN = 23;
static const uint8_t EPD_SCLK_PIN = 18;
static const uint8_t EPD_CS_PIN   = 27;
static const uint8_t EPD_DC_PIN   = 14;
static const uint8_t EPD_RST_PIN  = 33;
static const uint8_t EPD_BUSY_PIN = 13;

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

struct Pt { int16_t x; int16_t y; };

static const uint8_t DISPLAY_ZONE_COUNT = 5;

struct DisplayState {
  char title[64];
  char date[32];
  char time[16];
  bool masterEnable;
  bool weatherAdjustmentEnabled;
  char gardenNews[512];
  char displayMode[16];
  ZoneCfg zones[DISPLAY_ZONE_COUNT];
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
char relayBase[128] = "http://192.168.50.1";
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

const char* MODE_AUTO = "auto";
const char* MODE_SCHEDULE = "schedule";
const char* MODE_NEWS = "news";
const char* MODE_GRAPH = "graph";

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
  strlcpy(relayBase, prefs.getString("relayBase", "http://192.168.50.1").c_str(), sizeof(relayBase));
  strlcpy(relayApiToken, prefs.getString("relayApiToken", "").c_str(), sizeof(relayApiToken));
  strlcpy(state.displayMode, prefs.getString("displayMode", MODE_AUTO).c_str(), sizeof(state.displayMode));
  strlcpy(state.gardenNews, prefs.getString("gardenNews", "Welcome to Castle Hills Garden.").c_str(), sizeof(state.gardenNews));
  prefs.end();

  if (state.displayMode[0] == '\0') strlcpy(state.displayMode, MODE_AUTO, sizeof(state.displayMode));
  strlcpy(state.title, "Castle Hills Garden Watering Schedule", sizeof(state.title));
  strlcpy(state.date, "No relay time", sizeof(state.date));
  strlcpy(state.time, "--:--", sizeof(state.time));

  const char* defaults[DISPLAY_ZONE_COUNT] = {"Zone 1", "Zone 2", "Zone 3", "Zone 4", "Zone 5"};
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    if (state.zones[i].name[0] == '\0') strlcpy(state.zones[i].name, defaults[i], sizeof(state.zones[i].name));
    if (state.zones[i].baseMinutes == 0) state.zones[i].baseMinutes = 15;
  }
}

void setupWifi() {
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAPdisconnect(true);
  delay(100);

  bool apOk = false;
  if (strlen(apPass) >= 8) apOk = WiFi.softAP(apSsid, apPass);
  else apOk = WiFi.softAP(apSsid);

  IPAddress apIp = WiFi.softAPIP();
  dns.stop();
  dns.start(53, "*", apIp);

  String msg = String("Admin AP ") + (apOk ? "started" : "failed") + ": " + apSsid + " at " + apIp.toString();
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
  if (state.run.active) {
    strlcpy(state.title, "Castle Hills Garden Watering Schedule", sizeof(state.title));
    return;
  }
  if (strcmp(state.displayMode, MODE_AUTO) != 0) {
    if (strcmp(state.displayMode, MODE_NEWS) == 0) strlcpy(state.title, "Castle Hills Garden News", sizeof(state.title));
    else if (strcmp(state.displayMode, MODE_GRAPH) == 0) strlcpy(state.title, "Current + Weekly Weather", sizeof(state.title));
    else strlcpy(state.title, "Castle Hills Garden Watering Schedule", sizeof(state.title));
    return;
  }

  unsigned long cycleMs = (millis() - rotationEpochMs) % (4UL * 60UL * 1000UL);
  if (cycleMs < 45000UL) strlcpy(state.title, "Castle Hills Garden News", sizeof(state.title));
  else if (cycleMs >= 120000UL && cycleMs < 165000UL) strlcpy(state.title, "Current + Weekly Weather", sizeof(state.title));
  else strlcpy(state.title, "Castle Hills Garden Watering Schedule", sizeof(state.title));
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

bool fetchRunState(DynamicJsonDocument& sdoc) {
  DynamicJsonDocument first(8192);
  if (fetchRelayJson("/status", first, "Relay status /status")) {
    sdoc.clear();
    sdoc.set(first.as<JsonVariant>());
    return true;
  }

  char firstError[384];
  strlcpy(firstError, relayLastError, sizeof(firstError));

  DynamicJsonDocument second(8192);
  if (fetchRelayJson("/api/state", second, "Relay status /api/state")) {
    sdoc.clear();
    sdoc.set(second.as<JsonVariant>());
    return true;
  }

  setRelayStatus("Relay status", String("Both status endpoints failed. /status: ") + firstError + " /api/state: " + relayLastError, false);
  return false;
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

  time_t now = (time_t)epoch;
  struct tm* tmv = localtime(&now);
  const char* weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  snprintf(state.date, sizeof(state.date), "%s, %s %s", weekdays[tmv->tm_wday], months[tmv->tm_mon], ordinalDay(tmv->tm_mday).c_str());
  int h = tmv->tm_hour % 12;
  if (h == 0) h = 12;
  snprintf(state.time, sizeof(state.time), "%d:%02d %s", h, tmv->tm_min, tmv->tm_hour >= 12 ? "PM" : "AM");

  DynamicJsonDocument wdoc(4096);
  if (!fetchRelayJson("/weather", wdoc, "Relay weather")) return false;

  strlcpy(state.weather.summary, wdoc["summary"] | "", sizeof(state.weather.summary));
  strlcpy(state.weather.condition, wdoc["condition"] | "", sizeof(state.weather.condition));
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

  JsonObject run = sdoc["currentRun"].as<JsonObject>();
  state.run.active = run["active"] | false;
  state.run.zone = run["zone"] | 0;
  state.run.remainingSeconds = run["remainingSeconds"] | 0;
  state.run.totalSeconds = run["durationSeconds"] | run["totalSeconds"] | 0;

  JsonArray zones = sdoc["zones"].as<JsonArray>();
  if (!zones.isNull()) {
    int i = 0;
    for (JsonObject z : zones) {
      if (i >= DISPLAY_ZONE_COUNT) break;
      strlcpy(state.zones[i].name, z["name"] | state.zones[i].name, sizeof(state.zones[i].name));
      state.zones[i].baseMinutes = z["baseMinutes"] | state.zones[i].baseMinutes;
      state.zones[i].startHour = z["startHour"] | state.zones[i].startHour;
      state.zones[i].startMinute = z["startMinute"] | state.zones[i].startMinute;
      i++;
    }
  }

  relayDataReady = true;
  forceDiagnosticScreen = false;
  setRelayStatus("Relay data", "Relay data rendered successfully. Time, weather, and status are available.", true);
  return true;
}

const Pt Z1[] = {{24, 82}, {185, 82}, {185, 218}, {95, 218}, {24, 170}};
const Pt Z2[] = {{192, 82}, {320, 82}, {330, 180}, {230, 200}};
const Pt Z3[] = {{332, 82}, {420, 82}, {420, 220}, {332, 220}};
const Pt Z4[] = {{36, 232}, {190, 232}, {180, 410}, {22, 410}};
const Pt Z5[] = {{192, 210}, {420, 210}, {420, 420}, {192, 420}};

void drawPolyOutline(const Pt* p, int n) {
  for (int i = 0; i < n; i++) {
    const Pt& a = p[i];
    const Pt& b = p[(i + 1) % n];
    display.drawLine(a.x, a.y, b.x, b.y, GxEPD_BLACK);
  }
}

void fillPolyHatch(const Pt* p, int n, bool active) {
  int minY = 999, maxY = -1, minX = 999, maxX = -1;
  for (int i = 0; i < n; i++) {
    minY = min(minY, (int)p[i].y); maxY = max(maxY, (int)p[i].y);
    minX = min(minX, (int)p[i].x); maxX = max(maxX, (int)p[i].x);
  }
  if (!active) return;
  for (int yy = minY; yy <= maxY; yy += 5) display.drawLine(minX, yy, maxX, yy, GxEPD_BLACK);
}

void drawZoneLabel(int x, int y, int zone, bool active) {
  display.setFont(&FreeMono9pt7b);
  if (active) {
    display.fillRect(x - 2, y - 13, 58, 16, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
  } else {
    display.setTextColor(GxEPD_BLACK);
  }
  display.setCursor(x, y);
  display.printf("Zone %d", zone);
  display.setTextColor(GxEPD_BLACK);
}

void drawMap(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  bool a1 = state.run.active && state.run.zone == 1;
  bool a2 = state.run.active && state.run.zone == 2;
  bool a3 = state.run.active && state.run.zone == 3;
  bool a4 = state.run.active && state.run.zone == 4;
  bool a5 = state.run.active && state.run.zone == 5;

  fillPolyHatch(Z1, 5, a1); drawPolyOutline(Z1, 5); drawZoneLabel(34, 104, 1, a1);
  fillPolyHatch(Z2, 4, a2); drawPolyOutline(Z2, 4); drawZoneLabel(220, 104, 2, a2);
  fillPolyHatch(Z3, 4, a3); drawPolyOutline(Z3, 4); drawZoneLabel(340, 104, 3, a3);
  fillPolyHatch(Z4, 4, a4); drawPolyOutline(Z4, 4); drawZoneLabel(48, 258, 4, a4);
  fillPolyHatch(Z5, 4, a5); drawPolyOutline(Z5, 4); drawZoneLabel(220, 238, 5, a5);

  display.drawLine(x + 20, y + 360, x + 404, y + 360, GxEPD_BLACK);
  display.drawLine(x + 20, y + 385, x + 404, y + 385, GxEPD_BLACK);
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
  display.setCursor(x + 12, y + 145); display.print("Sunrise");
  display.setCursor(x + 12, y + 158); display.print(sunriseTxt);
  display.drawCircle(x + 180, y + 153, 78, GxEPD_BLACK);
  display.fillRect(x + 96, y + 153, 170, 34, GxEPD_WHITE);
  display.drawLine(x + 98, y + 153, x + 262, y + 153, GxEPD_BLACK);
  unsigned long nowEpoch = time(nullptr);
  if (state.weather.sunriseEpoch > 0 && state.weather.sunsetEpoch > state.weather.sunriseEpoch) {
    float pct = (float)((long)nowEpoch - (long)state.weather.sunriseEpoch) / (float)((long)state.weather.sunsetEpoch - (long)state.weather.sunriseEpoch);
    pct = constrain(pct, 0.0f, 1.0f);
    int sx = x + 100 + (int)(pct * 160.0f);
    float localNorm = ((float)sx - (float)(x + 180)) / 80.0f;
    int sy = y + 153 - (int)(sqrtf(max(0.0f, 1.0f - localNorm * localNorm)) * 24.0f);
    display.fillCircle(sx, sy, 4, GxEPD_BLACK);
  }
  display.setCursor(x + 278, y + 145); display.print("Sunset");
  display.setCursor(x + 282, y + 158); display.print(sunsetTxt);
}

void drawSchedulePanel(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.setFont(&FreeMonoBold12pt7b);
  display.setCursor(x + 8, y + 22);
  display.print("Schedule");
  display.setFont(&FreeSans9pt7b);
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    int col = i < 3 ? 0 : 1;
    int row = i % 3;
    int sx = x + 10 + col * 175;
    int sy = y + 52 + row * 25;
    int hour = state.zones[i].startHour % 12;
    if (hour == 0) hour = 12;
    display.setCursor(sx, sy);
    display.printf("Zone %d %d:%02d%s %um", i + 1, hour, state.zones[i].startMinute, state.zones[i].startHour >= 12 ? "pm" : "am", state.zones[i].baseMinutes);
  }
}

void drawRuntimePanel(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, GxEPD_WHITE);
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.setFont(&FreeMonoBold12pt7b);
  if (!state.run.active) {
    if (lastFinishedZone > 0) {
      display.setCursor(x + 8, y + 25); display.printf("Finished Zone %u", lastFinishedZone);
    } else {
      display.setCursor(x + 8, y + 25); display.print("Idle");
    }
    display.drawRect(x + 8, y + 42, w - 16, 20, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(x + 8, y + 88);
    if (!relayDataReady && relayLastError[0]) display.print("Relay error");
    else display.print("Idle");
    return;
  }

  display.setCursor(x + 8, y + 25);
  display.printf("Running Zone %u", state.run.zone);
  float r = state.run.totalSeconds ? ((float)(state.run.totalSeconds - state.run.remainingSeconds) / (float)state.run.totalSeconds) : 0;
  r = constrain(r, 0.0f, 1.0f);
  display.drawRect(x + 8, y + 42, w - 16, 20, GxEPD_BLACK);
  display.fillRect(x + 9, y + 43, (int)((w - 18) * r), 18, GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 8, y + 88);
  display.printf("Remaining: %um %us", state.run.remainingSeconds / 60, state.run.remainingSeconds % 60);
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

void renderScheduleScreenFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(8, 25);
    display.print("Castle Hills Garden Watering Schedule");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(570, 25); display.print(state.date);
    display.setCursor(690, 44); display.print(state.time);
    display.drawLine(8, 48, 792, 48, GxEPD_BLACK);
    drawMap(8, 48, 424, 424);
    drawWeatherWidget(432, 48, 360, 160);
    drawSchedulePanel(432, 207, 360, 133);
    drawRuntimePanel(432, 339, 360, 133);
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

void handleRoot() {
  String page;
  page.reserve(11000);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Garden E-Ink Admin</title><style>");
  page += F("body{font-family:Arial,sans-serif;margin:0;background:#f4f1e8;color:#111}header{background:#111;color:#fff;padding:14px 16px}h1{font-size:20px;margin:0}main{padding:12px;max-width:900px;margin:auto}section{background:#fff;border:1px solid #222;margin:12px 0;padding:12px;box-shadow:2px 2px 0 #222}h2{font-size:17px;margin:0 0 10px}label{display:block;margin:8px 0 3px;font-weight:700}input,textarea,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #333;background:#fff;font-size:15px}button,.btn{display:inline-block;margin:6px 6px 6px 0;padding:10px 12px;border:1px solid #111;background:#111;color:#fff;font-weight:700;text-decoration:none}.secondary{background:#fff;color:#111}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.status{white-space:pre-wrap;border:1px solid #333;padding:10px;background:#fafafa}.small{font-size:13px;color:#333}.ok{font-weight:700}.bad{font-weight:700;color:#8a0000}@media(max-width:650px){.grid{grid-template-columns:1fr}}");
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
  page += F("</div><a class='btn' href='/sync'>Sync Relay</a><a class='btn' href='/redraw'>Redraw E-Paper</a><a class='btn' href='/stop'>Stop / All Off</a></section>");

  page += F("<section><h2>Wi-Fi + Relay Token Settings</h2><p class='small'>Saving these settings reboots the display so Wi-Fi can restart cleanly. Reconnect to the admin AP after it comes back up.</p>");
  page += F("<form method='get' action='/saveConnectivity'><div class='grid'><div>");
  page += htmlInput("apSsid", "Admin AP SSID", apSsid, "text", "maxlength='31'");
  page += htmlInput("apPass", "Admin AP Password", "", "password", "maxlength='63' placeholder='leave blank to keep existing; 8+ chars to change'");
  page += F("</div><div>");
  page += htmlInput("staSsid", "Home / Station Wi-Fi SSID", staSsid, "text", "maxlength='31'");
  page += htmlInput("staPass", "Home / Station Wi-Fi Password", "", "password", "maxlength='63' placeholder='leave blank to keep existing'");
  page += F("</div></div>");
  page += htmlInput("relayBase", "Relay Base URL", relayBase, "text", "maxlength='127' placeholder='http://192.168.50.1'");
  page += htmlInput("relayApiToken", "Relay API Token", "", "password", "maxlength='95' placeholder='leave blank to keep existing / blank if unused'");
  page += F("<button type='submit'>Save Wi-Fi / Relay Settings</button></form></section>");

  page += F("<section><h2>Display Mode</h2><a class='btn' href='/display?mode=schedule'>Show Schedule</a><a class='btn' href='/display?mode=news'>Show News</a><a class='btn' href='/display?mode=graph'>Show Historic Weather</a><a class='btn secondary' href='/display?mode=auto'>Resume Auto Rotation</a></section>");

  page += F("<section><h2>Manual Extra Water</h2><form method='get' action='/extra'><label>Zone</label><select name='zone'><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select><label>Minutes</label><input name='minutes' type='number' value='10' min='1' max='240'><button type='submit'>Queue Extra Water</button></form></section>");

  page += F("<section><h2>Zones</h2><form method='get' action='/saveZone'><div class='grid'><div><label>Zone</label><select name='zone'><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select>");
  page += htmlInput("name", "Name", state.zones[0].name, "text", "maxlength='23'");
  page += htmlInput("baseMinutes", "Base Minutes", String(state.zones[0].baseMinutes), "number", "min='1' max='240'");
  page += F("</div><div>");
  page += htmlInput("startHour", "Start Hour 0-23", String(state.zones[0].startHour), "number", "min='0' max='23'");
  page += htmlInput("startMinute", "Start Minute 0-59", String(state.zones[0].startMinute), "number", "min='0' max='59'");
  page += F("<button type='submit'>Save Zone</button><p class='small'>This no-JavaScript form preloads Zone 1 values. Select another zone and enter its values manually.</p></div></div></form></section>");

  page += F("<section><h2>Full-Screen Garden News</h2><form method='get' action='/saveNewsForm'><textarea name='news' rows='5'>");
  page += htmlEscape(state.gardenNews);
  page += F("</textarea><button type='submit'>Save News to Display</button></form></section>");

  page += F("<section><h2>Weather History</h2><a class='btn' href='/history.csv'>Download CSV</a><a class='btn secondary' href='/clearHistory'>Clear History</a></section>");
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
  doc["weatherAdjustmentEnabled"] = state.weatherAdjustmentEnabled;
  doc["gardenNews"] = state.gardenNews;
  doc["currentRunActive"] = state.run.active;
  doc["currentRunZone"] = state.run.zone;
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

  JsonArray zones = doc.createNestedArray("zones");
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) {
    JsonObject z = zones.createNestedObject();
    z["name"] = state.zones[i].name;
    z["baseMinutes"] = state.zones[i].baseMinutes;
    z["startHour"] = state.zones[i].startHour;
    z["startMinute"] = state.zones[i].startMinute;
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
  if (m == "auto" || m == "schedule" || m == "news" || m == "graph") {
    strlcpy(state.displayMode, m.c_str(), sizeof(state.displayMode));
    saveConfig();
    forceDiagnosticScreen = !relayDataReady;
    forceFullRedraw = true;
  }
  server.send(200, "application/json", "{\"ok\":true}");
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

void handleExtra() {
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (zone < 1 || zone > DISPLAY_ZONE_COUNT || minutes < 1 || minutes > 240) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"zone must be 1-5 and minutes 1-240\"}");
    return;
  }
  if (!queueStopped) queueDepth++;
  pendingExtraZone = zone;
  pendingExtraMinutes = minutes;
  server.send(200, "application/json", "{\"ok\":true,\"queued\":true}");
}

void handleStop() {
  queueStopped = true;
  state.run.active = false;
  pendingExtraZone = 0;
  pendingExtraMinutes = 0;
  forceFullRedraw = true;
  server.send(200, "application/json", "{\"ok\":true,\"stopped\":true}");
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
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/saveConnectivity", HTTP_GET, handleSaveConnectivity);
  server.on("/saveNewsForm", HTTP_GET, handleSaveNewsForm);
  server.on("/extra", HTTP_GET, handleExtra);
  server.on("/stop", HTTP_GET, handleStop);
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
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Garden E-Ink Zone Display booting...");

  // E-paper display does not need MISO. Do not put GPIO12 into display SPI startup.
  SPI.begin(EPD_SCLK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);

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

  if (millis() - lastPollMs >= 15000UL) {
    lastPollMs = millis();
    syncFromRelay();

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
    } else if (relayDataReady && state.run.active && state.run.remainingSeconds != lastDrawn.run.remainingSeconds) {
      display.setPartialWindow(432, 339, 360, 133);
      display.firstPage();
      do {
        drawRuntimePanel(432, 339, 360, 133);
      } while (display.nextPage());
      lastDrawn.run.remainingSeconds = state.run.remainingSeconds;
    }
  }
}
