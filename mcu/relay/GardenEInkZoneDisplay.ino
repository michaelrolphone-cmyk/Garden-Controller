#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

// 7.5" 800x480 SPI e-paper panel on ESP32-C6 driver board.
// Pinout (adjust for your board revision): CS=5, DC=17, RST=16, BUSY=4, SD_CS=10.
static const uint8_t EPD_CS_PIN = 5;
static const uint8_t EPD_DC_PIN = 17;
static const uint8_t EPD_RST_PIN = 16;
static const uint8_t EPD_BUSY_PIN = 4;
static const uint8_t SD_CS_PIN = 10;

static const char* DEFAULT_AP_SSID = "GardenEInkDisplay";
static const char* DEFAULT_AP_PASS = "gardenpaper";
static const char* DEFAULT_API_BASE = "http://192.168.4.1:3000";

GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(GxEPD2_750_T7(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));
WebServer server(80);
DNSServer dns;
Preferences prefs;

struct ZoneRun { uint8_t zone; bool active; int remainingSeconds; int durationSeconds; };
struct ControllerState { static const uint8_t kMaxZones = 12; ZoneRun zones[kMaxZones]; uint8_t zoneCount; };
struct Point { int16_t x; int16_t y; };
struct ZonePolygon { uint8_t zone; const Point* points; uint8_t pointCount; };

ControllerState currentState = {};
bool needsFullRefresh = true;
unsigned long lastPollMs = 0;
char apSsid[32] = "GardenEInkDisplay";
char apPass[64] = "gardenpaper";
char staSsid[32] = "";
char staPass[64] = "";
char apiBase[128] = "http://192.168.4.1:3000";
char apiToken[96] = "";
String wifiStatus = "not configured";

const Point ZONE_1_POLY[] = {{40,80},{210,80},{240,170},{180,250},{60,220}};
const Point ZONE_2_POLY[] = {{245,80},{420,80},{420,230},{270,210}};
const Point ZONE_3_POLY[] = {{430,80},{620,80},{620,230},{430,230}};
const Point ZONE_4_POLY[] = {{70,260},{240,260},{240,430},{40,430}};
const Point ZONE_5_POLY[] = {{250,240},{620,240},{620,430},{250,430}};
const ZonePolygon ZONE_POLYGONS[] = {{1, ZONE_1_POLY, 5}, {2, ZONE_2_POLY, 4}, {3, ZONE_3_POLY, 4}, {4, ZONE_4_POLY, 4}, {5, ZONE_5_POLY, 4}};

float runtimeRatio(int remainingSeconds, int durationSeconds) { if (durationSeconds <= 0) return 0.0f; if (remainingSeconds <= 0) return 1.0f; float used = (float)(durationSeconds - remainingSeconds) / (float)durationSeconds; return constrain(used, 0.0f, 1.0f); }
const ZoneRun* findZoneRun(const ControllerState& state, uint8_t zone) { for (uint8_t i = 0; i < state.zoneCount; i++) if (state.zones[i].zone == zone) return &state.zones[i]; return nullptr; }

bool isSubstantialRedrawNeeded(const ControllerState& prev, const ControllerState& next) {
  if (prev.zoneCount != next.zoneCount) return true;
  for (uint8_t i = 0; i < next.zoneCount; i++) {
    const ZoneRun& a = prev.zones[i];
    const ZoneRun& b = next.zones[i];
    if (a.zone != b.zone) return true;
    if (a.active != b.active) return true; // zone map hatch changes => large area update
  }
  return false;
}

bool statesEqual(const ControllerState& a, const ControllerState& b) {
  if (a.zoneCount != b.zoneCount) return false;
  for (uint8_t i = 0; i < a.zoneCount; i++) {
    if (a.zones[i].zone != b.zones[i].zone) return false;
    if (a.zones[i].active != b.zones[i].active) return false;
    if (a.zones[i].remainingSeconds != b.zones[i].remainingSeconds) return false;
    if (a.zones[i].durationSeconds != b.zones[i].durationSeconds) return false;
  }
  return true;
}

void saveConfig() {
  prefs.begin("eink", false);
  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  prefs.putString("staSsid", staSsid);
  prefs.putString("staPass", staPass);
  prefs.putString("apiBase", apiBase);
  prefs.putString("apiToken", apiToken);
  prefs.end();
}

void loadConfig() {
  prefs.begin("eink", true);
  strlcpy(apSsid, prefs.getString("apSsid", DEFAULT_AP_SSID).c_str(), sizeof(apSsid));
  strlcpy(apPass, prefs.getString("apPass", DEFAULT_AP_PASS).c_str(), sizeof(apPass));
  strlcpy(staSsid, prefs.getString("staSsid", "").c_str(), sizeof(staSsid));
  strlcpy(staPass, prefs.getString("staPass", "").c_str(), sizeof(staPass));
  strlcpy(apiBase, prefs.getString("apiBase", DEFAULT_API_BASE).c_str(), sizeof(apiBase));
  strlcpy(apiToken, prefs.getString("apiToken", "").c_str(), sizeof(apiToken));
  prefs.end();
}

void setupApSta() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPass);
  dns.start(53, "*", WiFi.softAPIP());
  if (strlen(staSsid) > 0) {
    WiFi.begin(staSsid, staPass);
    wifiStatus = "connecting";
  } else wifiStatus = "home WiFi not configured";
}

void handleRoot() {
  String html = "<html><body><h1>Garden E-Ink Display</h1>";
  html += "<p>AP: " + String(apSsid) + " IP: " + WiFi.softAPIP().toString() + "</p>";
  html += "<p>WiFi status: " + wifiStatus + "</p>";
  html += "<p>Endpoints: /api/config (GET/POST), /api/state</p></body></html>";
  server.send(200, "text/html", html);
}

void handleApiConfigGet() {
  StaticJsonDocument<512> doc;
  doc["apSsid"] = apSsid;
  doc["staSsid"] = staSsid;
  doc["apiBase"] = apiBase;
  doc["apiToken"] = apiToken;
  doc["wifiStatus"] = wifiStatus;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiConfigSet() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}"); return; }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
  if (doc["apSsid"].is<const char*>()) strlcpy(apSsid, doc["apSsid"], sizeof(apSsid));
  if (doc["apPass"].is<const char*>()) strlcpy(apPass, doc["apPass"], sizeof(apPass));
  if (doc["staSsid"].is<const char*>()) strlcpy(staSsid, doc["staSsid"], sizeof(staSsid));
  if (doc["staPass"].is<const char*>()) strlcpy(staPass, doc["staPass"], sizeof(staPass));
  if (doc["apiBase"].is<const char*>()) strlcpy(apiBase, doc["apiBase"], sizeof(apiBase));
  if (doc["apiToken"].is<const char*>()) strlcpy(apiToken, doc["apiToken"], sizeof(apiToken));
  saveConfig();
  WiFi.disconnect(false, false);
  setupApSta();
  server.send(200, "application/json", "{\"ok\":true}");
}

bool fetchState(ControllerState& out) {
  if (strlen(apiToken) == 0) return false;
  HTTPClient http; String url = String(apiBase) + "/api/state";
  http.begin(url); http.addHeader("x-api-token", apiToken);
  int code = http.GET(); if (code != 200) { http.end(); return false; }
  DynamicJsonDocument doc(8192); auto err = deserializeJson(doc, http.getString()); http.end(); if (err) return false;
  JsonArray runs = doc["deviceTelemetry"]["zoneRuns"].as<JsonArray>();
  out.zoneCount = 0;
  for (JsonObject r : runs) {
    if (out.zoneCount >= ControllerState::kMaxZones) break;
    out.zones[out.zoneCount++] = {(uint8_t)(r["zone"] | 1), (bool)(r["active"] | false), (int)(r["remainingSeconds"] | 0), (int)(r["durationSeconds"] | 1800)};
  }
  return true;
}

void drawPolygonOutline(const ZonePolygon& poly) { for (uint8_t i = 0; i < poly.pointCount; i++) { Point a = poly.points[i]; Point b = poly.points[(i + 1) % poly.pointCount]; display.drawLine(a.x, a.y, b.x, b.y, GxEPD_BLACK);} }
void fillPolygonHatch(const ZonePolygon& poly, bool active) { int16_t minX=32767,maxX=-32767,minY=32767,maxY=-32767; for (uint8_t i=0;i<poly.pointCount;i++){minX=min(minX,poly.points[i].x);maxX=max(maxX,poly.points[i].x);minY=min(minY,poly.points[i].y);maxY=max(maxY,poly.points[i].y);} uint8_t step=active?6:12; for (int16_t y=minY;y<=maxY;y+=step) display.drawLine(minX,y,maxX,y,GxEPD_BLACK);}
void drawZoneLabelAndMeter(uint8_t slot, const ZoneRun* zoneRun) { int16_t panelX=640,panelY=80+slot*72; display.setCursor(panelX,panelY); display.printf("Zone %u",zoneRun?zoneRun->zone:(slot+1)); display.setCursor(panelX+86,panelY); display.print(zoneRun&&zoneRun->active?"ACTIVE":"Idle"); float ratio=zoneRun?runtimeRatio(zoneRun->remainingSeconds,zoneRun->durationSeconds):0.0f; display.drawRect(panelX,panelY+10,140,16,GxEPD_BLACK); display.fillRect(panelX+1,panelY+11,(int16_t)(138*ratio),14,GxEPD_BLACK); display.setCursor(panelX,panelY+42); display.printf("Remain: %ds",zoneRun?zoneRun->remainingSeconds:0); }
void renderZoneMap(const ControllerState& state) { for (uint8_t i=0;i<5;i++){ const ZonePolygon& poly=ZONE_POLYGONS[i]; const ZoneRun* run=findZoneRun(state, poly.zone); bool active=run&&run->active; fillPolygonHatch(poly,active); drawPolygonOutline(poly); Point p=poly.points[0]; display.setCursor(p.x+8,p.y+20); display.printf("Z%u", poly.zone);} }

void renderFullMap(const ControllerState& state) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(20, 30);
    display.print("Garden Zone Map");
    display.drawRect(20, 55, 610, 390, GxEPD_BLACK);
    renderZoneMap(state);
    display.setFont(&FreeMono9pt7b);
    for (uint8_t i = 0; i < 5; i++) drawZoneLabelAndMeter(i, findZoneRun(state, i + 1));
  } while (display.nextPage());
}

void renderPartialLegend(const ControllerState& state) {
  display.setPartialWindow(630, 55, 170, 390);
  display.firstPage();
  do {
    display.fillRect(630, 55, 170, 390, GxEPD_WHITE);
    display.setFont(&FreeMono9pt7b);
    for (uint8_t i = 0; i < 5; i++) drawZoneLabelAndMeter(i, findZoneRun(state, i + 1));
  } while (display.nextPage());
}

void logStateToSd(const ControllerState& state) { if (!SD.begin(SD_CS_PIN)) return; File f = SD.open("/zone_state.log", FILE_APPEND); if (!f) return; f.printf("ms=%lu zones=%u\n", millis(), state.zoneCount); for (uint8_t i=0;i<state.zoneCount;i++) f.printf("zone=%u active=%d remaining=%d\n", state.zones[i].zone, state.zones[i].active, state.zones[i].remainingSeconds); f.close(); }

void handleApiState() { StaticJsonDocument<256> d; d["wifiStatus"] = wifiStatus; d["zoneCount"] = currentState.zoneCount; String out; serializeJson(d,out); server.send(200,"application/json",out);} 

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigSet);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  loadConfig();
  display.init();
  setupApSta();
  setupServer();
  needsFullRefresh = true;
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) wifiStatus = "connected:" + WiFi.localIP().toString();

  if (millis() - lastPollMs >= 15000) {
    lastPollMs = millis();
    ControllerState next = {};
    if (fetchState(next)) {
      bool changed = !statesEqual(currentState, next);
      if (changed) {
        logStateToSd(next);
        if (needsFullRefresh || isSubstantialRedrawNeeded(currentState, next)) {
          renderFullMap(next);
          needsFullRefresh = false;
        } else {
          renderPartialLegend(next);
        }
        currentState = next;
      }
    }
  }
}
