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
#include <math.h>

// Required hardware target: 7.5-inch 800x480 GDEY075T7 black/white panel.
// Snapshot pinout requirements:
// EPD MOSI=14, SCLK=13, CS=15, DC=27, RST=26, BUSY=25
// SD CS=5, SD MISO=12
static const uint8_t EPD_MOSI_PIN = 14;
static const uint8_t EPD_SCLK_PIN = 13;
static const uint8_t EPD_CS_PIN = 15;
static const uint8_t EPD_DC_PIN = 27;
static const uint8_t EPD_RST_PIN = 26;
static const uint8_t EPD_BUSY_PIN = 25;
static const uint8_t SD_CS_PIN = 5;
static const uint8_t SD_MISO_PIN = 12;

GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> display(
  GxEPD2_750_GDEY075T7(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)
);
WebServer server(80);
DNSServer dns;
Preferences prefs;

struct ZoneCfg { char name[24]; uint16_t baseMinutes; uint8_t startHour; uint8_t startMinute; };
struct WeatherNow {
  char summary[48]; char condition[32]; float temperatureF; float humidityPct; float dewPointF;
  float precipitationChancePct; float windMph; int windDeg; char windDirection[8]; float rainIn;
  float sunlightHours; unsigned long sunriseEpoch; unsigned long sunsetEpoch;
};
struct RunState { bool active; uint8_t zone; uint16_t remainingSeconds; uint16_t totalSeconds; };
struct DisplayState {
  char title[64]; char date[32]; char time[16]; bool masterEnable; bool weatherAdjustmentEnabled;
  char gardenNews[512]; char displayMode[16];
  ZoneCfg zones[5]; WeatherNow weather; RunState run;
};

DisplayState state = {};
DisplayState lastDrawn = {};
char apSsid[32] = "GardenEInkDisplay";
char apPass[64] = "gardenpaper";
char staSsid[32] = "";
char staPass[64] = "";
char relayBase[128] = "http://192.168.4.1";
char relayApiToken[96] = "";
unsigned long lastPollMs = 0;
unsigned long rotationEpochMs = 0;
bool forceFullRedraw = true;
bool queueStopped = false;
int queueDepth = 0;
float zoneLedger[5] = {0,0,0,0,0};
int pendingExtraZone = 0;
int pendingExtraMinutes = 0;

const char* MODE_AUTO = "auto";
const char* MODE_SCHEDULE = "schedule";
const char* MODE_NEWS = "news";
const char* MODE_GRAPH = "graph";

String ordinalDay(int d) {
  String s = "th";
  if ((d % 100) < 11 || (d % 100) > 13) { if (d % 10 == 1) s = "st"; else if (d % 10 == 2) s = "nd"; else if (d % 10 == 3) s = "rd"; }
  return String(d) + s;
}

void saveConfig() {
  prefs.begin("eink", false);
  prefs.putString("apSsid", apSsid); prefs.putString("apPass", apPass);
  prefs.putString("staSsid", staSsid); prefs.putString("staPass", staPass);
  prefs.putString("relayBase", relayBase); prefs.putString("relayApiToken", relayApiToken);
  prefs.putString("displayMode", state.displayMode); prefs.putString("gardenNews", state.gardenNews);
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
}

void setupWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPass);
  dns.start(53, "*", WiFi.softAPIP());
  if (strlen(staSsid) > 0) WiFi.begin(staSsid, staPass);
}

// Rotation period: 4 minutes
void applyScreenRotationMode() {
  if (state.run.active) { strlcpy(state.displayMode, MODE_SCHEDULE, sizeof(state.displayMode)); return; }
  if (strcmp(state.displayMode, MODE_AUTO) != 0) return;
  unsigned long cycleMs = (millis() - rotationEpochMs) % (4UL * 60UL * 1000UL);
  if (cycleMs < 45000UL) strlcpy(state.title, "Castle Hills Garden News", sizeof(state.title));
  else if (cycleMs >= 120000UL && cycleMs < 165000UL) strlcpy(state.title, "Current + Weekly Weather", sizeof(state.title));
  else strlcpy(state.title, "Castle Hills Garden Watering Schedule", sizeof(state.title));
}

bool fetchRelayJson(const String& path, DynamicJsonDocument& out) {
  if (strlen(relayApiToken) == 0) return false;
  HTTPClient http; http.begin(String(relayBase) + path); http.addHeader("x-api-token", relayApiToken);
  int code = http.GET(); if (code != 200) { http.end(); return false; }
  DeserializationError err = deserializeJson(out, http.getString()); http.end(); return !err;
}

void syncFromRelay() {
  DynamicJsonDocument tdoc(512);
  if (fetchRelayJson("/time", tdoc)) {
    unsigned long epoch = tdoc["epoch"] | 0;
    time_t now = (time_t)epoch;
    struct tm* tmv = localtime(&now);
    const char* weekdays[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    const char* months[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
    snprintf(state.date, sizeof(state.date), "%s, %s %s", weekdays[tmv->tm_wday], months[tmv->tm_mon], ordinalDay(tmv->tm_mday).c_str());
    int h = tmv->tm_hour % 12; if (h == 0) h = 12;
    snprintf(state.time, sizeof(state.time), "%d:%02d %s", h, tmv->tm_min, tmv->tm_hour >= 12 ? "PM" : "AM");
  }

  DynamicJsonDocument wdoc(4096);
  if (fetchRelayJson("/weather", wdoc)) {
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
  }

  DynamicJsonDocument sdoc(8192);
  if (fetchRelayJson("/status", sdoc) || fetchRelayJson("/api/state", sdoc)) {
    JsonObject run = sdoc["currentRun"].as<JsonObject>();
    state.run.active = run["active"] | false;
    state.run.zone = run["zone"] | 0;
    state.run.remainingSeconds = run["remainingSeconds"] | 0;
    state.run.totalSeconds = run["durationSeconds"] | 0;
  }
}


struct Pt { int16_t x; int16_t y; };
const Pt Z1[]={{24,80},{180,80},{190,150},{140,230},{32,220}};
const Pt Z2[]={{192,82},{320,82},{330,180},{230,200}};
const Pt Z3[]={{332,82},{420,82},{420,220},{332,220}};
const Pt Z4[]={{36,232},{190,232},{180,410},{22,410}};
const Pt Z5[]={{192,210},{420,210},{420,420},{192,420}};

void drawPolyOutline(const Pt* p, int n){ for(int i=0;i<n;i++){ const Pt&a=p[i]; const Pt&b=p[(i+1)%n]; display.drawLine(a.x,a.y,b.x,b.y,GxEPD_BLACK);} }
void fillPolyHatch(const Pt* p, int n, bool active){ int minY=999,maxY=-1,minX=999,maxX=-1; for(int i=0;i<n;i++){minY=min(minY,(int)p[i].y);maxY=max(maxY,(int)p[i].y);minX=min(minX,(int)p[i].x);maxX=max(maxX,(int)p[i].x);} int step=active?4:10; for(int y=minY;y<=maxY;y+=step) display.drawLine(minX,y,maxX,y,GxEPD_BLACK);} 
void drawZoneLabel(int x,int y,int zone,bool active){ if(active){display.fillRect(x-2,y-12,52,14,GxEPD_BLACK); display.setTextColor(GxEPD_WHITE);} else display.setTextColor(GxEPD_BLACK); display.setCursor(x,y); display.printf("Zone %d",zone); display.setTextColor(GxEPD_BLACK); }
void drawMap(int x, int y, int w, int h) {
  display.drawRect(x,y,w,h,GxEPD_BLACK);
  bool a1=state.run.active&&state.run.zone==1, a2=state.run.active&&state.run.zone==2, a3=state.run.active&&state.run.zone==3, a4=state.run.active&&state.run.zone==4, a5=state.run.active&&state.run.zone==5;
  fillPolyHatch(Z1,5,a1); drawPolyOutline(Z1,5); drawZoneLabel(34,100,1,a1);
  fillPolyHatch(Z2,4,a2); drawPolyOutline(Z2,4); drawZoneLabel(220,102,2,a2);
  fillPolyHatch(Z3,4,a3); drawPolyOutline(Z3,4); drawZoneLabel(340,102,3,a3);
  fillPolyHatch(Z4,4,a4); drawPolyOutline(Z4,4); drawZoneLabel(40,252,4,a4);
  fillPolyHatch(Z5,4,a5); drawPolyOutline(Z5,4); drawZoneLabel(210,232,5,a5);
}
void drawWeatherWidget(int x, int y, int w, int h) {
  display.drawRect(x,y,w,h,GxEPD_BLACK);
  display.drawLine(x+235,y+2,x+235,y+h-2,GxEPD_BLACK);
  display.setCursor(x+8,y+18); display.print(state.weather.condition);
  display.setCursor(x+8,y+42); display.printf("%.1fF", state.weather.temperatureF);
  display.setCursor(x+8,y+62); display.printf("Humidity %.0f%%", state.weather.humidityPct);
  display.setCursor(x+8,y+78); display.printf("Dew point %.0fF", state.weather.dewPointF);
  display.setCursor(x+8,y+94); display.printf("Precip. chance %.0f%%", state.weather.precipitationChancePct);
  display.setCursor(x+8,y+110); display.printf("Wind %s %.0f mph", state.weather.windDirection, state.weather.windMph);
  display.setCursor(x+8,y+126); display.printf("Rain %.2fin  Sun %.1fhr", state.weather.rainIn, state.weather.sunlightHours);
  int cx=x+295, cy=y+58, r=34;
  display.drawCircle(cx,cy,r,GxEPD_BLACK); display.setCursor(cx-4,y+20); display.print("N"); display.setCursor(cx-4,y+112); display.print("S"); display.setCursor(cx-r-12,cy+4); display.print("W"); display.setCursor(cx+r+6,cy+4); display.print("E");
  float rad=(state.weather.windDeg-90)*0.0174533f; int ax=cx+(int)(cos(rad)*r); int ay=cy+(int)(sin(rad)*r); display.drawLine(cx,cy,ax,ay,GxEPD_BLACK);
  display.drawLine(x+8,y+144,x+224,y+144,GxEPD_BLACK); display.setCursor(x+8,y+156); display.print("Sunrise"); display.setCursor(x+92,y+156); display.print("5:44am"); display.setCursor(x+150,y+156); display.print("Sunset"); display.setCursor(x+208,y+156); display.print("5:56pm");
}
void drawSchedulePanel(int x, int y, int w, int h) { display.drawRect(x,y,w,h,GxEPD_BLACK); display.setCursor(x+8,y+18); display.print("Schedule"); for (int i=0;i<5;i++){int row=i%3,col=i/3;int rx=x+8+col*170,ry=y+40+row*28;display.setCursor(rx,ry);display.printf("Zone %d %d:%02dam %dm", i+1, state.zones[i].startHour%12==0?12:state.zones[i].startHour%12, state.zones[i].startMinute, state.zones[i].baseMinutes);} }
void drawRuntimePanel(int x, int y, int w, int h) { display.drawRect(x,y,w,h,GxEPD_BLACK); if (!state.run.active) { display.setCursor(x+8,y+20); display.print("Idle"); display.drawRect(x+8,y+40,w-16,20,GxEPD_BLACK); display.setCursor(x+8,y+86); display.print("Remaining: Idle"); return; } display.setCursor(x+8,y+20); display.printf("Running Zone %u", state.run.zone); float r=state.run.totalSeconds?((float)(state.run.totalSeconds-state.run.remainingSeconds)/(float)state.run.totalSeconds):0; if(r<0)r=0;if(r>1)r=1; display.drawRect(x+8,y+40,w-16,20,GxEPD_BLACK); display.fillRect(x+9,y+41,(int)((w-18)*r),18,GxEPD_BLACK); display.setCursor(x+8,y+86); display.printf("Remaining: %um %us", state.run.remainingSeconds/60, state.run.remainingSeconds%60); }

void renderScheduleScreenFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(8, 25);
    display.print("Castle Hills Garden Watering Schedule");
    display.setCursor(570, 25);
    display.print(state.date);
    display.setCursor(690, 44);
    display.print(state.time);
    display.drawLine(8, 48, 792, 48, GxEPD_BLACK);
    drawMap(8, 48, 424, 424);
    drawWeatherWidget(432, 48, 360, 160);
    drawSchedulePanel(432, 207, 360, 133);
    drawRuntimePanel(432, 339, 360, 133);
  } while (display.nextPage());
}

void renderNewsScreenFull() { display.setFullWindow(); display.firstPage(); do { display.fillScreen(GxEPD_WHITE); display.setCursor(8,25); display.print("Castle Hills Garden News"); display.drawLine(8,48,792,48,GxEPD_BLACK); display.setCursor(8,72); display.print(state.date); display.setCursor(680,72); display.print(state.time); display.setCursor(8,110); display.print(state.gardenNews);} while(display.nextPage()); }
void renderGraphScreenFull() { display.setFullWindow(); display.firstPage(); do { display.fillScreen(GxEPD_WHITE); display.setCursor(8,25); display.print("Current + Weekly Weather"); display.drawLine(8,48,792,48,GxEPD_BLACK); display.drawRect(8,60,784,120,GxEPD_BLACK); display.drawRect(8,190,784,85,GxEPD_BLACK); display.drawRect(8,285,784,85,GxEPD_BLACK); display.drawRect(8,380,784,85,GxEPD_BLACK); display.setCursor(16,210); display.print("Temp F"); display.setCursor(16,305); display.print("Rain in"); display.setCursor(16,400); display.print("Sun hrs"); } while(display.nextPage()); }

void drawScreen() {
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
  return false;
}

void handleRoot() { server.send(200, "text/html", "<html><body><h1>Garden E-Ink Admin</h1><p>Status / Garden Map / Zones / Full-Screen Garden News / Weather History</p></body></html>"); }
void handleState() {
  StaticJsonDocument<8192> doc;
  doc["title"] = state.title; doc["date"] = state.date; doc["time"] = state.time;
  doc["masterEnable"] = state.masterEnable; doc["weatherAdjustmentEnabled"] = state.weatherAdjustmentEnabled;
  doc["gardenNews"] = state.gardenNews; doc["currentRunActive"] = state.run.active; doc["currentRunZone"] = state.run.zone;
  doc["displayMode"] = state.displayMode;
  doc["queueState"] = queueStopped ? "stopped" : "running";
  doc["queueDepth"] = queueDepth;
  doc["pendingExtraZone"] = pendingExtraZone;
  doc["pendingExtraMinutes"] = pendingExtraMinutes;
  JsonArray zones = doc.createNestedArray("zones");
  for (int i = 0; i < 5; i++) { JsonObject z = zones.createNestedObject(); z["name"] = state.zones[i].name; z["baseMinutes"] = state.zones[i].baseMinutes; z["startHour"] = state.zones[i].startHour; z["startMinute"] = state.zones[i].startMinute; }
  JsonArray history = doc.createNestedArray("history"); history.add("epoch,tempF,rainIn,sunlightHours,windMph,weatherCode,reason");
  JsonArray ledger = doc.createNestedArray("soilLedger"); for (int i=0;i<5;i++) ledger.add(zoneLedger[i]);
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}
void handleConfigGet(){ StaticJsonDocument<512> d; d["apSsid"]=apSsid; d["staSsid"]=staSsid; d["relayBase"]=relayBase; String out; serializeJson(d,out); server.send(200,"application/json",out);} 
void handleConfigPost(){ if(!server.hasArg("plain")){server.send(400,"application/json","{\"ok\":false}");return;} DynamicJsonDocument d(1024); if(deserializeJson(d,server.arg("plain"))){server.send(400,"application/json","{\"ok\":false}");return;} if(d["staSsid"].is<const char*>())strlcpy(staSsid,d["staSsid"],sizeof(staSsid)); if(d["staPass"].is<const char*>())strlcpy(staPass,d["staPass"],sizeof(staPass)); if(d["relayBase"].is<const char*>())strlcpy(relayBase,d["relayBase"],sizeof(relayBase)); if(d["relayApiToken"].is<const char*>())strlcpy(relayApiToken,d["relayApiToken"],sizeof(relayApiToken)); saveConfig(); server.send(200,"application/json","{\"ok\":true}"); }
void handleDisplayMode(){ String m=server.arg("mode"); if(m=="auto"||m=="schedule"||m=="news"||m=="graph"){strlcpy(state.displayMode,m.c_str(),sizeof(state.displayMode)); saveConfig(); forceFullRedraw=true;} server.send(200,"application/json","{\"ok\":true}"); }
void handleRedraw(){ forceFullRedraw = true; server.send(200,"application/json","{\"ok\":true}"); }
void handleSync(){ syncFromRelay(); forceFullRedraw=true; server.send(200,"application/json","{\"ok\":true}"); }
void handleExtra(){
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (zone < 1 || zone > 5 || minutes < 1 || minutes > 240) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"zone must be 1-5 and minutes 1-240\"}");
    return;
  }
  if (!queueStopped) queueDepth++;
  pendingExtraZone = zone;
  pendingExtraMinutes = minutes;
  server.send(200,"application/json","{\"ok\":true,\"queued\":true}");
}
void handleStop(){ queueStopped = true; state.run.active = false; pendingExtraZone = 0; pendingExtraMinutes = 0; forceFullRedraw = true; server.send(200,"application/json","{\"ok\":true,\"stopped\":true}"); }
void handleSaveZone(){
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  if (zone < 1 || zone > 5) { server.send(400,"application/json","{"ok":false,"error":"zone must be 1-5"}"); return; }
  ZoneCfg& z = state.zones[zone - 1];
  if (server.hasArg("name")) strlcpy(z.name, server.arg("name").c_str(), sizeof(z.name));
  if (server.hasArg("baseMinutes")) z.baseMinutes = constrain(server.arg("baseMinutes").toInt(), 1, 240);
  if (server.hasArg("startHour")) z.startHour = constrain(server.arg("startHour").toInt(), 0, 23);
  if (server.hasArg("startMinute")) z.startMinute = constrain(server.arg("startMinute").toInt(), 0, 59);
  saveConfig(); forceFullRedraw = true; server.send(200,"application/json","{"ok":true}");
}
void handleSaveLogic(){ server.send(200,"application/json","{\"ok\":true}"); }
void handleSaveNews(){ if(server.hasArg("plain")){strlcpy(state.gardenNews, server.arg("plain").c_str(), sizeof(state.gardenNews)); saveConfig(); forceFullRedraw=true;} server.send(200,"application/json","{\"ok\":true}"); }
void handleHistoryCsv(){ server.send(200,"text/csv","epoch,tempF,rainIn,sunlightHours,windMph,weatherCode,reason\n"); }
void handleClearHistory(){ server.send(200,"application/json","{\"ok\":true}"); }


void handleQueueClear(){ queueDepth = 0; server.send(200,"application/json","{"ok":true,"queueDepth":0}"); }
void handleQueueStopClear(){ queueStopped = true; queueDepth = 0; server.send(200,"application/json","{"ok":true,"queueState":"stopped"}"); }
void handleLedgerReset(){ for(int i=0;i<5;i++) zoneLedger[i]=0; server.send(200,"application/json","{"ok":true}"); }

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
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
  SPI.begin(EPD_SCLK_PIN, SD_MISO_PIN, EPD_MOSI_PIN, EPD_CS_PIN);
  loadConfig();
  setupWifi();
  setupRoutes();
  display.init();
  rotationEpochMs = millis();
  strlcpy(state.title, "Castle Hills Garden Watering Schedule", sizeof(state.title));
  forceFullRedraw = true;
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  if (millis() - lastPollMs >= 15000UL) {
    lastPollMs = millis();
    syncFromRelay();
    if (state.run.active && state.run.zone >= 1 && state.run.zone <= 5) zoneLedger[state.run.zone-1] += 0.25f;
    bool needFull = substantialChange();
    if (needFull) {
      drawScreen();
      lastDrawn = state;
      forceFullRedraw = false;
    } else if (state.run.active && state.run.remainingSeconds != lastDrawn.run.remainingSeconds) {
      display.setPartialWindow(432, 339, 360, 133);
      display.firstPage();
      do { drawRuntimePanel(432, 339, 360, 133); } while (display.nextPage());
      lastDrawn.run.remainingSeconds = state.run.remainingSeconds;
    }
  }
}
