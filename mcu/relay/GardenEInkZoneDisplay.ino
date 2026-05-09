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
static const uint8_t DISPLAY_ZONE_COUNT = 5;
struct DisplayState {
  char title[64]; char date[32]; char time[16]; bool masterEnable; bool weatherAdjustmentEnabled;
  char gardenNews[512]; char displayMode[16];
  ZoneCfg zones[DISPLAY_ZONE_COUNT]; WeatherNow weather; RunState run;
};
struct HistoryRow { unsigned long epoch; float tempF; float rainIn; float sunlightHours; float windMph; int weatherCode; char reason[24]; };
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
unsigned long lastPollMs = 0;
unsigned long rotationEpochMs = 0;
bool forceFullRedraw = true;
bool queueStopped = false;
int queueDepth = 0;
float zoneLedger[5] = {0,0,0,0,0};
int pendingExtraZone = 0;
int pendingExtraMinutes = 0;
uint8_t lastFinishedZone = 0;

const char* MODE_AUTO = "auto";
const char* MODE_SCHEDULE = "schedule";
const char* MODE_NEWS = "news";
const char* MODE_GRAPH = "graph";

String ordinalDay(int d) {
  String s = "th";
  if ((d % 100) < 11 || (d % 100) > 13) { if (d % 10 == 1) s = "st"; else if (d % 10 == 2) s = "nd"; else if (d % 10 == 3) s = "rd"; }
  return String(d) + s;
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
void formatTimeLowerNoLeadingZero(unsigned long epoch, char* out, size_t outSize) {
  if (!epoch) { snprintf(out, outSize, "--:--"); return; }
  time_t t = (time_t)epoch;
  struct tm* tmv = localtime(&t);
  int hour = tmv->tm_hour % 12; if (hour == 0) hour = 12;
  snprintf(out, outSize, "%d:%02d%s", hour, tmv->tm_min, tmv->tm_hour >= 12 ? "pm" : "am");
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
  display.drawLine(x+128,y+2,x+128,y+130,GxEPD_BLACK);
  display.drawLine(x+246,y+2,x+246,y+130,GxEPD_BLACK);
  display.drawLine(x+2,y+132,x+w-2,y+132,GxEPD_BLACK);
  display.drawCircle(x+24,y+20,10,GxEPD_BLACK);
  display.drawLine(x+24,y+7,x+24,y+33,GxEPD_BLACK);
  display.drawLine(x+11,y+20,x+37,y+20,GxEPD_BLACK);
  char clippedCondition[20]; snprintf(clippedCondition, sizeof(clippedCondition), "%.18s", state.weather.condition);
  display.setCursor(x+40,y+20); display.print(clippedCondition);
  display.setCursor(x+8,y+48); display.printf("%.0fF", state.weather.temperatureF);
  display.fillCircle(x+136,y+18,2,GxEPD_BLACK); display.setCursor(x+144,y+20); display.printf("Humidity %.0f%%", state.weather.humidityPct);
  display.drawRect(x+132,y+30,5,10,GxEPD_BLACK); display.setCursor(x+144,y+38); display.printf("Dew point %.0fF", state.weather.dewPointF);
  display.drawLine(x+132,y+53,x+138,y+59,GxEPD_BLACK); display.drawLine(x+138,y+53,x+132,y+59,GxEPD_BLACK); display.setCursor(x+144,y+56); display.printf("Precip. chance %.0f%%", state.weather.precipitationChancePct);
  display.drawLine(x+132,y+72,x+138,y+72,GxEPD_BLACK); display.drawLine(x+134,y+69,x+140,y+69,GxEPD_BLACK); display.setCursor(x+144,y+74); display.printf("Wind %s", state.weather.windDirection);
  display.fillCircle(x+134,y+90,2,GxEPD_BLACK); display.setCursor(x+144,y+92); display.printf("%.0f mph", state.weather.windMph);
  display.drawCircle(x+134,y+108,3,GxEPD_BLACK); display.setCursor(x+144,y+110); display.printf("Rain %.2fin", state.weather.rainIn);
  display.drawCircle(x+134,y+126,3,GxEPD_BLACK); display.drawLine(x+134,y+120,x+134,y+132,GxEPD_BLACK); display.setCursor(x+144,y+128); display.printf("Sun %.1fhr", state.weather.sunlightHours);
  int cx=x+303, cy=y+62, r=44;
  display.drawCircle(cx,cy,r,GxEPD_BLACK); display.setCursor(cx-4,y+20); display.print("N"); display.setCursor(cx-4,y+112); display.print("S"); display.setCursor(cx-r-12,cy+4); display.print("W"); display.setCursor(cx+r+6,cy+4); display.print("E");
  float windDeg = state.weather.windDeg == 0 && strcmp(state.weather.windDirection, "N") != 0 ? directionToDegrees(state.weather.windDirection) : (float)state.weather.windDeg;
  float rad=(windDeg-90)*0.0174533f; int ax=cx+(int)(cos(rad)*r); int ay=cy+(int)(sin(rad)*r); display.fillTriangle(cx,cy, cx+(int)(cos(rad+2.75f)*16), cy+(int)(sin(rad+2.75f)*16), cx+(int)(cos(rad-2.75f)*16), cy+(int)(sin(rad-2.75f)*16), GxEPD_BLACK); display.drawLine(cx,cy,ax,ay,GxEPD_BLACK);
  display.setCursor(cx-14, cy+4); display.print(state.weather.windDirection);
  display.setCursor(cx-20, cy+16); display.printf("%.0f mph", state.weather.windMph);
  char sunriseTxt[12]; char sunsetTxt[12];
  formatTimeLowerNoLeadingZero(state.weather.sunriseEpoch, sunriseTxt, sizeof(sunriseTxt));
  formatTimeLowerNoLeadingZero(state.weather.sunsetEpoch, sunsetTxt, sizeof(sunsetTxt));
  display.setCursor(x+6,y+145); display.print("Sunrise");
  display.setCursor(x+10,y+158); display.print(sunriseTxt);
  for (int hx = x+94; hx <= x+266; hx += 8) display.drawLine(hx, y+154, hx+4, y+154, GxEPD_BLACK);
  for (int px = 0; px <= 172; px++) { float t = (float)px / 172.0f; float yy = sinf(t * 3.14159f); int sy = y + 154 - (int)(yy * 28.0f); display.drawPixel(x+94+px, sy, GxEPD_BLACK); }
  unsigned long nowEpoch = time(nullptr);
  if (state.weather.sunriseEpoch > 0 && state.weather.sunsetEpoch > state.weather.sunriseEpoch) {
    float pct = (float)((long)nowEpoch - (long)state.weather.sunriseEpoch) / (float)((long)state.weather.sunsetEpoch - (long)state.weather.sunriseEpoch);
    if (pct < 0.0f) pct = 0.0f; if (pct > 1.0f) pct = 1.0f;
    int sx = x + 94 + (int)(pct * 172.0f);
    float yy = sinf((float)(sx - (x + 94)) / 172.0f * 3.14159f);
    int sy = y + 154 - (int)(yy * 28.0f);
    display.fillCircle(sx, sy, 4, GxEPD_BLACK);
  }
  display.setCursor(x+278,y+145); display.print("Sunset");
  display.setCursor(x+282,y+158); display.print(sunsetTxt);
}
void drawSchedulePanel(int x, int y, int w, int h) { display.drawRect(x,y,w,h,GxEPD_BLACK); display.setCursor(x+8,y+18); display.print("Schedule"); for (int i=0;i<DISPLAY_ZONE_COUNT;i++){int row=i%3,col=i/3;int rx=x+8+col*170,ry=y+40+row*28;display.setCursor(rx,ry);display.printf("Zone %d %d:%02dam %dm", i+1, state.zones[i].startHour%12==0?12:state.zones[i].startHour%12, state.zones[i].startMinute, state.zones[i].baseMinutes);} }
void drawRuntimePanel(int x, int y, int w, int h) { display.drawRect(x,y,w,h,GxEPD_BLACK); if (!state.run.active) { if (lastFinishedZone > 0) { display.setCursor(x+8,y+20); display.printf("Finished Zone %u", lastFinishedZone); } else { display.setCursor(x+8,y+20); display.print("Idle"); } display.drawRect(x+8,y+40,w-16,20,GxEPD_BLACK); display.setCursor(x+8,y+86); display.print("Idle"); return; } display.setCursor(x+8,y+20); display.printf("Running Zone %u", state.run.zone); float r=state.run.totalSeconds?((float)(state.run.totalSeconds-state.run.remainingSeconds)/(float)state.run.totalSeconds):0; if(r<0)r=0;if(r>1)r=1; display.drawRect(x+8,y+40,w-16,20,GxEPD_BLACK); display.fillRect(x+9,y+41,(int)((w-18)*r),18,GxEPD_BLACK); display.setCursor(x+8,y+86); display.printf("Remaining: %um %us", state.run.remainingSeconds/60, state.run.remainingSeconds%60); }

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


void drawWrappedTextBlock(int x, int y, int maxWidth, int lineHeight, const char* text) {
  if (!text || !*text) return;
  const int maxCharsPerLine = max(24, maxWidth / 11);
  char working[512];
  strlcpy(working, text, sizeof(working));
  char* token = strtok(working, " ");
  char line[128] = {0};
  int currentY = y;
  while (token) {
    if (strlen(line) == 0) {
      strlcpy(line, token, sizeof(line));
    } else if ((int)(strlen(line) + 1 + strlen(token)) <= maxCharsPerLine) {
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
  if (strlen(line) > 0) {
    display.setCursor(x, currentY);
    display.print(line);
  }
}

void renderNewsScreenFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(8,25); display.print("Castle Hills Garden News");
    display.drawLine(8,48,792,48,GxEPD_BLACK);
    display.drawRect(8,58,784,404,GxEPD_BLACK);
    display.setCursor(16,82); display.print(state.date);
    display.setCursor(650,82); display.print(state.time);
    display.drawLine(16,92,784,92,GxEPD_BLACK);
    drawWrappedTextBlock(16, 120, 760, 20, state.gardenNews);
  } while(display.nextPage());
}

void drawGraphFrame(int x, int y, int w, int h, const char* title, const char* yTop, const char* yMid, const char* yBot, const char* xLeft, const char* xRight) {
  display.drawRect(x,y,w,h,GxEPD_BLACK);
  display.setCursor(x+8,y+16); display.print(title);
  display.drawLine(x+72,y+20,x+72,y+h-14,GxEPD_BLACK);
  display.drawLine(x+72,y+h-14,x+w-12,y+h-14,GxEPD_BLACK);
  display.setCursor(x+8,y+34); display.print(yTop);
  display.setCursor(x+8,y+h/2); display.print(yMid);
  display.setCursor(x+8,y+h-16); display.print(yBot);
  display.setCursor(x+74,y+h-2); display.print(xLeft);
  display.setCursor(x+w-40,y+h-2); display.print(xRight);
}

int loadFakeHistory(HistoryRow* rows, int maxRows) {
  if (!rows || maxRows <= 0) return 0;
  char csv[768]; strlcpy(csv, HISTORY_CSV_DATA, sizeof(csv));
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
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
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
  for (int i = 0; i < count; i++) if (rows[i].rainIn > maxRain) maxRain = rows[i].rainIn;
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
    display.setCursor(8,25); display.print("Current + Weekly Weather");
    display.drawLine(8,48,792,48,GxEPD_BLACK);
    display.setCursor(16,72); display.print(state.date);
    display.setCursor(680,72); display.print(state.time);
    display.drawRect(8,84,784,84,GxEPD_BLACK);
    display.setCursor(16,106); display.printf("Now %.0fF %s", state.weather.temperatureF, state.weather.condition);
    display.setCursor(16,126); display.printf("Rain %.2fin  Sun %.1fhr  Wind %.0fmph", state.weather.rainIn, state.weather.sunlightHours, state.weather.windMph);
    display.drawRect(8,176,784,50,GxEPD_BLACK);
    display.setCursor(16,202); display.print("7-day forecast strip");
    display.drawRect(8,230,784,50,GxEPD_BLACK);
    display.setCursor(16,256); display.print("8-slot hourly forecast strip");
    drawGraphFrame(8,285,784,60,"Temp F","90","70","50","Start","End");
    drawGraphFrame(8,350,784,60,"Rain in","1.0","0.5","0.0","Start","End");
    drawGraphFrame(8,415,784,50,"Sun hrs","12","6","0","Start","End");
    HistoryRow rows[8];
    int rowCount = loadFakeHistory(rows, 8);
    drawHistorySeriesLine(rows, rowCount, 84, 306, 696, 24, true);
    drawHistorySeriesRainBars(rows, rowCount, 84, 371, 696, 24);
    drawHistorySeriesLine(rows, rowCount, 84, 431, 696, 18, false);
  } while(display.nextPage());
}

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

void handleRoot() {
  server.send(200, "text/html",
    "<html><body><h1>Garden E-Ink Admin</h1>"
    "<p>Status / Garden Map / Zones / Full-Screen Garden News / Weather History</p>"
    "<h2>Display Mode</h2>"
    "<button onclick=\"fetch('/display?mode=schedule').then(()=>location.reload())\">Show Schedule</button>"
    "<button onclick=\"fetch('/display?mode=news').then(()=>location.reload())\">Show News</button>"
    "<button onclick=\"fetch('/display?mode=graph').then(()=>location.reload())\">Show Historic Weather</button>"
    "<button onclick=\"fetch('/display?mode=auto').then(()=>location.reload())\">Resume Auto Rotation</button>"
    "<h2>Manual Extra Water</h2>"
    "<label>Zone selector <select id='zone'>"
    "<option value='1'>1</option><option value='2'>2</option><option value='3'>3</option><option value='4'>4</option><option value='5'>5</option>"
    "</select></label>"
    "<label>Minutes input <input id='minutes' type='number' min='1' max='240' value='10'></label>"
    "<button onclick=\"fetch('/extra?zone='+encodeURIComponent(document.getElementById('zone').value)+'&minutes='+encodeURIComponent(document.getElementById('minutes').value)).then(()=>location.reload())\">Run Extra Water</button>"
    "</body></html>");
}
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
  for (int i = 0; i < DISPLAY_ZONE_COUNT; i++) { JsonObject z = zones.createNestedObject(); z["name"] = state.zones[i].name; z["baseMinutes"] = state.zones[i].baseMinutes; z["startHour"] = state.zones[i].startHour; z["startMinute"] = state.zones[i].startMinute; }
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
  if (zone < 1 || zone > 5) { server.send(400,"application/json","{\"ok\":false,\"error\":\"zone must be 1-5\"}"); return; }
  ZoneCfg& z = state.zones[zone - 1];
  if (server.hasArg("name")) strlcpy(z.name, server.arg("name").c_str(), sizeof(z.name));
  if (server.hasArg("baseMinutes")) z.baseMinutes = constrain(server.arg("baseMinutes").toInt(), 1, 240);
  if (server.hasArg("startHour")) z.startHour = constrain(server.arg("startHour").toInt(), 0, 23);
  if (server.hasArg("startMinute")) z.startMinute = constrain(server.arg("startMinute").toInt(), 0, 59);
  saveConfig(); forceFullRedraw = true; server.send(200,"application/json","{\"ok\":true}");
}
void handleSaveLogic(){ server.send(200,"application/json","{\"ok\":true}"); }
void handleSaveNews(){ if(server.hasArg("plain")){strlcpy(state.gardenNews, server.arg("plain").c_str(), sizeof(state.gardenNews)); saveConfig(); forceFullRedraw=true;} server.send(200,"application/json","{\"ok\":true}"); }
void handleHistoryCsv(){
  server.send(200,"text/csv",HISTORY_CSV_DATA);
}
void handleClearHistory(){ server.send(200,"application/json","{\"ok\":true}"); }


void handleQueueClear(){ queueDepth = 0; server.send(200,"application/json","{\"ok\":true,\"queueDepth\":0}"); }
void handleQueueStopClear(){ queueStopped = true; queueDepth = 0; server.send(200,"application/json","{\"ok\":true,\"queueState\":\"stopped\"}"); }
void handleLedgerReset(){ for(int i=0;i<5;i++) zoneLedger[i]=0; server.send(200,"application/json","{\"ok\":true}"); }

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
    static bool previousRunActive = false;
    static uint8_t previousRunZone = 0;
    if (previousRunActive && !state.run.active && previousRunZone > 0) lastFinishedZone = previousRunZone;
    if (state.run.active && state.run.zone > 0) { previousRunZone = state.run.zone; lastFinishedZone = 0; }
    previousRunActive = state.run.active;
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
