#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>

// GardenSimpleRelay6.ino
// Simple 6-zone relay watering controller for Waveshare Industrial ESP32-S3 6CH relay board.
// - no weather algorithm
// - simple local schedule
// - AP + station WiFi
// - mobile map/admin UI
// - Heroku Garden-Controller API integration

static const uint8_t ZONE_COUNT = 5;
static const uint8_t RELAY_COUNT = 6;
static const uint8_t MASTER_RELAY_CHANNEL = 6;
static const uint8_t MAX_DAILY_SCHEDULES = 64;
static const uint8_t MASTER_RELAY_INDEX = MASTER_RELAY_CHANNEL - 1;
static const uint16_t DEFAULT_MANUAL_RUN_SECONDS = 15 * 60;
static const uint16_t DEFAULT_SPIGOT_RUN_MINUTES = 15;
static const uint8_t RELAY_PINS[RELAY_COUNT] = {1, 2, 41, 42, 45, 46};
static const uint8_t RELAY_ON = HIGH;
static const uint8_t RELAY_OFF = LOW;

static const bool ENABLE_BUZZER = true;
static const uint8_t BUZZER_PIN = 21;
static const uint8_t RGB_LED_PIN_R = 38;
static const uint8_t RGB_LED_PIN_G = 39;
static const uint8_t RGB_LED_PIN_B = 40;
// Passive buzzer volume reduction by duty cycle.
// Prior chirp used ~50% duty. 5% duty is roughly 1/10 of that drive time.
static const uint8_t BUZZER_DUTY_PERCENT = 5;

WebServer server(80);
DNSServer dns;
Preferences prefs;

IPAddress apIp(192, 168, 4, 1);
IPAddress apGw(192, 168, 4, 1);
IPAddress apMask(255, 255, 255, 0);

char apSsid[32] = "GardenRelay6";
char apPass[64] = "gardenwater";
char staSsid[32] = "";
char staPass[64] = "";

bool remoteEnabled = true;
char remoteApiBase[128] = "https://garden-controller-896690a38ea4.herokuapp.com";
char remoteDeviceId[48] = "garden-relay-6";
char remoteApiKey[96] = "CAFEFE";
uint16_t remoteIntervalSeconds = 15;
uint32_t lastRemoteRelayPublishMs = 0;
uint32_t lastRemoteSchedulePublishMs = 0;
uint32_t lastRemoteCommandPollMs = 0;
uint32_t lastRemoteSensorPublishMs = 0;
String lastRemoteStatus = "remote disabled";

static const uint16_t REMOTE_LONG_POLL_WAIT_SECONDS = 25;
static const uint32_t REMOTE_GET_TIMEOUT_MS = 30000UL;
static const uint32_t REMOTE_POST_TIMEOUT_MS = 10000UL;
static const uint32_t REMOTE_TELEMETRY_INTERVAL_MS = 15000UL;
static const uint32_t REMOTE_SENSOR_INTERVAL_MS = 300000UL;
static const uint32_t REMOTE_OFFLINE_DELAY_MS = 5000UL;

// Defaults only. Runtime location/timezone are configurable and persisted.
// The server should resolve timezone from coordinates and send both IANA and ESP32 POSIX TZ.
static const double DEFAULT_GARDEN_LATITUDE = 43.665288;
static const double DEFAULT_GARDEN_LONGITUDE = -116.259186;
static const char DEFAULT_GARDEN_TIME_ZONE[] = "America/Boise";
static const char DEFAULT_GARDEN_POSIX_TZ[] = "MST7MDT,M3.2.0,M11.1.0";

double gardenLatitude = DEFAULT_GARDEN_LATITUDE;
double gardenLongitude = DEFAULT_GARDEN_LONGITUDE;
char gardenTimeZone[48] = "America/Boise";
char gardenPosixTimeZone[64] = "MST7MDT,M3.2.0,M11.1.0";
TaskHandle_t remoteTaskHandle = nullptr;

const char FIRMWARE_VERSION[] = "v25-multi-zone-runs-compile-fixed";
char lastCommandId[48] = "";
char lastCommandStatus[16] = "";

struct ZoneSchedule {
  bool enabled;
  char name[24];
};

struct DailySchedule {
  bool enabled;
  uint8_t zoneIndex;
  uint8_t startHour;
  uint8_t startMinute;
  uint16_t runMinutes;
  int lastRunYearDay;
  int lastRunMinuteOfDay;
};

struct RunState {
  bool active;
  uint8_t zoneIndex;
  uint32_t startedMs;
  uint32_t durationMs;
  bool manual;
};

struct SpigotRunState {
  bool active;
  uint32_t startedMs;
  uint32_t durationMs;
};

ZoneSchedule zones[ZONE_COUNT];
DailySchedule dailySchedules[MAX_DAILY_SCHEDULES];
uint8_t dailyScheduleCount = 0;
bool relayState[RELAY_COUNT] = {false, false, false, false, false, false};
RunState zoneRuns[ZONE_COUNT];
SpigotRunState spigotRun = {false, 0, 0};

uint32_t lastWifiAttemptMs = 0;
uint32_t staConnectStartMs = 0;
bool staConnectInProgress = false;
String lastStaStatus = "not configured";

// Forward declarations
void setupGardenTimeZone();
void setupAp();
void connectSta(bool wait);
void setupServer();
void sendStateJson();
void handleRoot();
void handleAdmin();
void handleSaveAdmin();
void handleScheduleAdd();
void handleScheduleDelete();
void handleSetTime();
void handleRelay();
void handleManualRun();
void handleSpigotRun();
void handleAllOff();
void handleBuzzerTest();
void handleFactoryReset();
void handleApiFeatures();
void handleApiConfigGet();
void handleApiConfigSet();
void handleApiScheduleSet();
void handleRemoteConfig();
void handleRemoteTest();
void publishRelayStateNow();
void publishSchedulesNow();
void publishFullStateNow();
void publishSensorDataNow();
void clearDailySchedules();
bool addDailySchedule(uint8_t zoneIndex, uint8_t hour, uint8_t minute, uint16_t runMinutes, bool enabled = true);
String scheduleLine(uint8_t i);
void parseScheduleText(const String& textValue);
void defaultConfig();
void saveConfig();
void loadConfig();
void factoryReset();
bool parseStartTimeToZone(const char* startTime, uint8_t& hourOut, uint8_t& minuteOut);
bool ackRemoteCommand(const char* commandId, const char* status);
void remoteTask(void* param);

String two(int v) {
  return v < 10 ? "0" + String(v) : String(v);
}

String compactTime(uint8_t hour24, uint8_t minute) {
  uint8_t h = hour24 % 24;
  const char* suffix = h >= 12 ? "pm" : "am";
  uint8_t h12 = h % 12;
  if (h12 == 0) h12 = 12;
  return String(h12) + ":" + two(minute) + suffix;
}

String htmlTimeValue(uint8_t hour24, uint8_t minute) {
  return two(hour24 % 24) + ":" + two(minute);
}

String clockString() {
  struct tm t;
  if (!getLocalTime(&t, 20)) return "--";
  return compactTime(t.tm_hour, t.tm_min);
}

String dateString() {
  struct tm t;
  if (!getLocalTime(&t, 20)) return "No clock";
  const char* weekdays[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  const char* months[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
  int d = t.tm_mday;
  String suffix = "th";
  if ((d % 100) < 11 || (d % 100) > 13) {
    if (d % 10 == 1) suffix = "st";
    else if (d % 10 == 2) suffix = "nd";
    else if (d % 10 == 3) suffix = "rd";
  }
  return String(weekdays[t.tm_wday]) + ", " + months[t.tm_mon] + " " + String(d) + suffix;
}

bool clockIsValid() {
  time_t now;
  time(&now);
  return now > 1700000000UL;
}

void setupGardenTimeZone() {
  // The watering schedule is interpreted in the garden's local civil time,
  // not in browser/client time and not in UTC.
  setenv("TZ", gardenPosixTimeZone, 1);
  tzset();
}

String htmlEscape(const String& input) {
  String out;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void chirp(uint16_t freqHz = 2200, uint16_t durationMs = 45) {
  if (!ENABLE_BUZZER || freqHz == 0 || BUZZER_DUTY_PERCENT == 0) return;

  uint32_t periodUs = 1000000UL / freqHz;
  uint32_t highUs = (periodUs * BUZZER_DUTY_PERCENT) / 100UL;
  if (highUs < 2) highUs = 2;

  uint32_t lowUs = periodUs > highUs ? periodUs - highUs : 2;
  uint32_t endAt = micros() + (uint32_t)durationMs * 1000UL;

  while ((int32_t)(micros() - endAt) < 0) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(highUs);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(lowUs);
  }

  digitalWrite(BUZZER_PIN, LOW);
}


void setRelay(uint8_t relayIndex, bool on) {
  if (relayIndex >= RELAY_COUNT) return;
  bool wasOn = relayState[relayIndex];
  relayState[relayIndex] = on;
  digitalWrite(RELAY_PINS[relayIndex], on ? RELAY_ON : RELAY_OFF);
  if (on && !wasOn) chirp(2200 + relayIndex * 80, 45);
}

bool anyZoneRelayOn() {
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (relayState[i]) return true;
  }
  return false;
}

struct ZoneColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

const ZoneColor ZONE_COLORS[ZONE_COUNT] = {
  {59, 130, 246},
  {34, 197, 94},
  {245, 158, 11},
  {168, 85, 247},
  {236, 72, 153}
};

void setRgbLed(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(RGB_LED_PIN_R, r);
  analogWrite(RGB_LED_PIN_G, g);
  analogWrite(RGB_LED_PIN_B, b);
}

void updateZoneRgbLed() {
  uint8_t active[ZONE_COUNT];
  uint8_t activeCount = 0;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (relayState[i]) active[activeCount++] = i;
  }
  if (activeCount == 0) {
    setRgbLed(0, 0, 0);
    return;
  }
  uint32_t nowBucket = millis() / 1000UL;
  uint8_t colorIndex = active[nowBucket % activeCount];
  const ZoneColor& color = ZONE_COLORS[colorIndex];
  setRgbLed(color.r, color.g, color.b);
}

void updateMasterValve() {
  bool masterNeeded = anyZoneRelayOn() || spigotRun.active;
  setRelay(MASTER_RELAY_INDEX, masterNeeded);
}

void setZoneRelay(uint8_t zoneIndex, bool on) {
  if (zoneIndex >= ZONE_COUNT) return;
  setRelay(zoneIndex, on);
  updateMasterValve();
  updateZoneRgbLed();
}


void clearDailySchedules() {
  dailyScheduleCount = 0;
  for (uint8_t i = 0; i < MAX_DAILY_SCHEDULES; i++) {
    dailySchedules[i].enabled = false;
    dailySchedules[i].zoneIndex = 0;
    dailySchedules[i].startHour = 0;
    dailySchedules[i].startMinute = 0;
    dailySchedules[i].runMinutes = 0;
    dailySchedules[i].lastRunYearDay = -1;
    dailySchedules[i].lastRunMinuteOfDay = -1;
  }
}

bool addDailySchedule(uint8_t zoneIndex, uint8_t hour, uint8_t minute, uint16_t runMinutes, bool enabled) {
  if (zoneIndex >= ZONE_COUNT || hour > 23 || minute > 59 || runMinutes == 0 || dailyScheduleCount >= MAX_DAILY_SCHEDULES) return false;

  DailySchedule& s = dailySchedules[dailyScheduleCount++];
  s.enabled = enabled;
  s.zoneIndex = zoneIndex;
  s.startHour = hour;
  s.startMinute = minute;
  s.runMinutes = runMinutes;
  s.lastRunYearDay = -1;
  s.lastRunMinuteOfDay = -1;
  return true;
}

String scheduleLine(uint8_t i) {
  if (i >= dailyScheduleCount) return "";
  DailySchedule& s = dailySchedules[i];
  return String(s.zoneIndex + 1) + "," + two(s.startHour) + ":" + two(s.startMinute) + "," + String(s.runMinutes) + "," + String(s.enabled ? "on" : "off");
}

void parseScheduleText(const String& textValue) {
  clearDailySchedules();

  int start = 0;
  while (start < (int)textValue.length() && dailyScheduleCount < MAX_DAILY_SCHEDULES) {
    int end = textValue.indexOf('\n', start);
    if (end < 0) end = textValue.length();

    String line = textValue.substring(start, end);
    line.trim();
    line.replace("\r", "");

    if (line.length() > 0 && !line.startsWith("#")) {
      int c1 = line.indexOf(',');
      int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
      int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;

      if (c1 > 0 && c2 > c1) {
        int zoneNumber = line.substring(0, c1).toInt();
        String timeText = line.substring(c1 + 1, c2);
        String durationText = c3 > c2 ? line.substring(c2 + 1, c3) : line.substring(c2 + 1);
        String enabledText = c3 > c2 ? line.substring(c3 + 1) : "on";
        timeText.trim();
        durationText.trim();
        enabledText.trim();
        enabledText.toLowerCase();

        uint8_t h = 0;
        uint8_t m = 0;
        if (parseStartTimeToZone(timeText.c_str(), h, m)) {
          int runMinutes = durationText.toInt();
          bool enabled = !(enabledText == "off" || enabledText == "false" || enabledText == "0" || enabledText == "disabled");
          if (zoneNumber >= 1 && zoneNumber <= ZONE_COUNT && runMinutes >= 1) {
            addDailySchedule((uint8_t)(zoneNumber - 1), h, m, (uint16_t)constrain(runMinutes, 1, 240), enabled);
          }
        }
      }
    }

    start = end + 1;
  }
}

void defaultConfig() {
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    zones[i].enabled = true;
    snprintf(zones[i].name, sizeof(zones[i].name), "Zone %u", i + 1);
  }

  clearDailySchedules();
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    addDailySchedule(i, 6, (i * 10) % 60, 10, true);
  }

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    zoneRuns[i].active = false;
    zoneRuns[i].zoneIndex = i;
    zoneRuns[i].startedMs = 0;
    zoneRuns[i].durationMs = 0;
    zoneRuns[i].manual = false;
  }
}

void saveConfig() {
  prefs.begin("relay6", false);
  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  prefs.putString("staSsid", staSsid);
  prefs.putString("staPass", staPass);
  prefs.putBool("remoteEn", remoteEnabled);
  prefs.putString("remoteBase", remoteApiBase);
  prefs.putString("remoteId", remoteDeviceId);
  prefs.putString("remoteKey", remoteApiKey);
  prefs.putUShort("remoteInt", remoteIntervalSeconds);
  prefs.putDouble("gardenLat", gardenLatitude);
  prefs.putDouble("gardenLon", gardenLongitude);
  prefs.putString("gardenTz", gardenTimeZone);
  prefs.putString("gardenPosix", gardenPosixTimeZone);

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    String p = "z" + String(i);
    prefs.putBool((p + "en").c_str(), zones[i].enabled);
    prefs.putString((p + "name").c_str(), zones[i].name);
  }

  prefs.putUChar("schCount", dailyScheduleCount);
  for (uint8_t i = 0; i < MAX_DAILY_SCHEDULES; i++) {
    String p = "s" + String(i);
    if (i < dailyScheduleCount) {
      prefs.putBool((p + "en").c_str(), dailySchedules[i].enabled);
      prefs.putUChar((p + "zone").c_str(), dailySchedules[i].zoneIndex);
      prefs.putUChar((p + "h").c_str(), dailySchedules[i].startHour);
      prefs.putUChar((p + "m").c_str(), dailySchedules[i].startMinute);
      prefs.putUShort((p + "dur").c_str(), dailySchedules[i].runMinutes);
    }
  }
  prefs.end();
}

void loadConfig() {
  defaultConfig();
  prefs.begin("relay6", true);
  String s;
  s = prefs.getString("apSsid", apSsid); strlcpy(apSsid, s.c_str(), sizeof(apSsid));
  s = prefs.getString("apPass", apPass); strlcpy(apPass, s.c_str(), sizeof(apPass));
  s = prefs.getString("staSsid", staSsid); strlcpy(staSsid, s.c_str(), sizeof(staSsid));
  s = prefs.getString("staPass", staPass); strlcpy(staPass, s.c_str(), sizeof(staPass));
  remoteEnabled = prefs.getBool("remoteEn", remoteEnabled);
  s = prefs.getString("remoteBase", remoteApiBase); strlcpy(remoteApiBase, s.c_str(), sizeof(remoteApiBase));
  s = prefs.getString("remoteId", remoteDeviceId); strlcpy(remoteDeviceId, s.c_str(), sizeof(remoteDeviceId));
  s = prefs.getString("remoteKey", remoteApiKey); strlcpy(remoteApiKey, s.c_str(), sizeof(remoteApiKey));
  remoteIntervalSeconds = prefs.getUShort("remoteInt", remoteIntervalSeconds);
  gardenLatitude = prefs.getDouble("gardenLat", gardenLatitude);
  gardenLongitude = prefs.getDouble("gardenLon", gardenLongitude);
  s = prefs.getString("gardenTz", gardenTimeZone); strlcpy(gardenTimeZone, s.c_str(), sizeof(gardenTimeZone));
  s = prefs.getString("gardenPosix", gardenPosixTimeZone); strlcpy(gardenPosixTimeZone, s.c_str(), sizeof(gardenPosixTimeZone));

  if (strlen(gardenTimeZone) == 0) strlcpy(gardenTimeZone, DEFAULT_GARDEN_TIME_ZONE, sizeof(gardenTimeZone));
  if (strlen(gardenPosixTimeZone) == 0) strlcpy(gardenPosixTimeZone, DEFAULT_GARDEN_POSIX_TZ, sizeof(gardenPosixTimeZone));

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    String p = "z" + String(i);
    zones[i].enabled = prefs.getBool((p + "en").c_str(), zones[i].enabled);
    s = prefs.getString((p + "name").c_str(), zones[i].name); strlcpy(zones[i].name, s.c_str(), sizeof(zones[i].name));
  }

  uint8_t savedScheduleCount = prefs.getUChar("schCount", dailyScheduleCount);
  if (savedScheduleCount > MAX_DAILY_SCHEDULES) savedScheduleCount = MAX_DAILY_SCHEDULES;
  clearDailySchedules();
  for (uint8_t i = 0; i < savedScheduleCount; i++) {
    String p = "s" + String(i);
    bool enabled = prefs.getBool((p + "en").c_str(), true);
    uint8_t zoneIndex = prefs.getUChar((p + "zone").c_str(), 0);
    uint8_t hour = prefs.getUChar((p + "h").c_str(), 6);
    uint8_t minute = prefs.getUChar((p + "m").c_str(), 0);
    uint16_t dur = prefs.getUShort((p + "dur").c_str(), 10);
    addDailySchedule(zoneIndex, hour, minute, dur, enabled);
  }
  if (dailyScheduleCount == 0) {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) addDailySchedule(i, 6, (i * 10) % 60, 10, true);
  }
  prefs.end();

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    zoneRuns[i].active = false;
    zoneRuns[i].zoneIndex = i;
    zoneRuns[i].startedMs = 0;
    zoneRuns[i].durationMs = 0;
    zoneRuns[i].manual = false;
  }
}

void factoryReset() {
  prefs.begin("relay6", false);
  prefs.clear();
  prefs.end();
  strlcpy(apSsid, "GardenRelay6", sizeof(apSsid));
  strlcpy(apPass, "gardenwater", sizeof(apPass));
  strlcpy(staSsid, "", sizeof(staSsid));
  strlcpy(staPass, "", sizeof(staPass));
  remoteEnabled = true;
  strlcpy(remoteApiBase, "https://garden-controller-896690a38ea4.herokuapp.com", sizeof(remoteApiBase));
  strlcpy(remoteDeviceId, "garden-relay-6", sizeof(remoteDeviceId));
  strlcpy(remoteApiKey, "CAFEFE", sizeof(remoteApiKey));
  remoteIntervalSeconds = 15;
  gardenLatitude = DEFAULT_GARDEN_LATITUDE;
  gardenLongitude = DEFAULT_GARDEN_LONGITUDE;
  strlcpy(gardenTimeZone, DEFAULT_GARDEN_TIME_ZONE, sizeof(gardenTimeZone));
  strlcpy(gardenPosixTimeZone, DEFAULT_GARDEN_POSIX_TZ, sizeof(gardenPosixTimeZone));
  defaultConfig();
  saveConfig();
}

void allZonesOff() {
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    setRelay(i, false);
    zoneRuns[i].active = false;
  }
  updateMasterValve();
}

void allOff() {
  spigotRun.active = false;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    zoneRuns[i].active = false;
  }
  for (uint8_t i = 0; i < RELAY_COUNT; i++) setRelay(i, false);
}

bool anyZoneRunActive() {
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (zoneRuns[i].active) return true;
  }
  return false;
}

uint8_t activeZoneRunCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (zoneRuns[i].active) count++;
  }
  return count;
}

void startSpigotRunSeconds(uint32_t seconds) {
  if (seconds == 0) return;
  spigotRun.active = true;
  spigotRun.startedMs = millis();
  spigotRun.durationMs = seconds * 1000UL;
  updateMasterValve();
}

void startSpigotRun(uint16_t minutes) {
  if (minutes == 0) return;
  startSpigotRunSeconds((uint32_t)minutes * 60UL);
}

void stopSpigotRun() {
  spigotRun.active = false;
  updateMasterValve();
}

void startRunSeconds(uint8_t zoneIndex, uint32_t seconds, bool manual) {
  if (zoneIndex >= ZONE_COUNT || seconds == 0) return;

  zoneRuns[zoneIndex].active = true;
  zoneRuns[zoneIndex].zoneIndex = zoneIndex;
  zoneRuns[zoneIndex].startedMs = millis();
  zoneRuns[zoneIndex].durationMs = seconds * 1000UL;
  zoneRuns[zoneIndex].manual = manual;

  // Multiple zones may run together. Do not call allZonesOff() here.
  setZoneRelay(zoneIndex, true);
}

void startRun(uint8_t zoneIndex, uint16_t minutes, bool manual) {
  if (zoneIndex >= ZONE_COUNT || minutes == 0) return;
  startRunSeconds(zoneIndex, (uint32_t)minutes * 60UL, manual);
}

void stopRun() {
  allZonesOff();
  updateMasterValve();
}

void stopZone(uint8_t zoneIndex) {
  if (zoneIndex >= ZONE_COUNT) return;
  zoneRuns[zoneIndex].active = false;
  setZoneRelay(zoneIndex, false);
  updateMasterValve();
}

uint32_t commandDurationSeconds(JsonObject command) {
  int durationSeconds = command["durationSeconds"] | 0;
  if (durationSeconds <= 0) durationSeconds = DEFAULT_MANUAL_RUN_SECONDS;
  return (uint32_t)constrain(durationSeconds, 1, 240 * 60);
}


void updateRunState() {
  bool changed = false;

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    if (!zoneRuns[i].active) continue;

    uint32_t elapsed = millis() - zoneRuns[i].startedMs;
    if (elapsed >= zoneRuns[i].durationMs) {
      zoneRuns[i].active = false;
      setZoneRelay(i, false);
      changed = true;
    }
  }

  if (spigotRun.active) {
    uint32_t elapsed = millis() - spigotRun.startedMs;
    if (elapsed >= spigotRun.durationMs) {
      stopSpigotRun();
      changed = true;
    }
  }

  if (changed) {
    updateMasterValve();
    publishRelayStateNow();
    publishFullStateNow();
  }
}



void checkSchedule() {
  if (!clockIsValid()) return;

  struct tm t;
  if (!getLocalTime(&t, 20)) return;

  bool startedAny = false;
  int minuteOfDay = t.tm_hour * 60 + t.tm_min;
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& s = dailySchedules[i];
    if (!s.enabled || s.runMinutes == 0 || s.zoneIndex >= ZONE_COUNT || !zones[s.zoneIndex].enabled) continue;

    int schedMinute = s.startHour * 60 + s.startMinute;
    if (minuteOfDay == schedMinute) {
      if (s.lastRunYearDay != t.tm_yday || s.lastRunMinuteOfDay != minuteOfDay) {
        s.lastRunYearDay = t.tm_yday;
        s.lastRunMinuteOfDay = minuteOfDay;
        startRun(s.zoneIndex, s.runMinutes, false);
        startedAny = true;
      }
    }
  }

  if (startedAny) {
    publishRelayStateNow();
    publishFullStateNow();
  }
}



String remoteBaseNoSlash() {
  String b = String(remoteApiBase);
  b.trim();
  while (b.endsWith("/")) b.remove(b.length() - 1);
  return b;
}

String remoteUrl(const String& suffix) {
  return remoteBaseNoSlash() + suffix;
}

bool httpBeginUrl(HTTPClient& http, WiFiClient& plainClient, WiFiClientSecure& secureClient, const String& url) {
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }
  return http.begin(plainClient, url);
}

void addRemoteHeaders(HTTPClient& http) {
  http.addHeader("Content-Type", "application/json");
  if (strlen(remoteApiKey) > 0) http.addHeader("x-api-token", remoteApiKey);
}

bool remoteReady() {
  return remoteEnabled && strlen(remoteApiBase) > 0 && strlen(remoteApiKey) > 0 && WiFi.status() == WL_CONNECTED;
}

bool remotePostJson(const String& suffix, const String& body, String& responseOut, int& httpCodeOut) {
  responseOut = "";
  httpCodeOut = -999;
  if (!remoteReady()) {
    lastRemoteStatus = remoteEnabled ? "remote not ready / home WiFi disconnected" : "remote disabled";
    return false;
  }

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  String url = remoteUrl(suffix);
  if (!httpBeginUrl(http, plainClient, secureClient, url)) {
    lastRemoteStatus = "http begin failed";
    return false;
  }

  addRemoteHeaders(http);
  http.setTimeout(REMOTE_POST_TIMEOUT_MS);
  httpCodeOut = http.POST(body);
  if (httpCodeOut > 0) responseOut = http.getString();
  http.end();

  lastRemoteStatus = "POST " + suffix + " -> " + String(httpCodeOut);
  return httpCodeOut >= 200 && httpCodeOut < 300;
}

bool remoteGetJson(const String& suffix, String& responseOut, int& httpCodeOut) {
  responseOut = "";
  httpCodeOut = -999;
  if (!remoteReady()) {
    lastRemoteStatus = remoteEnabled ? "remote not ready / home WiFi disconnected" : "remote disabled";
    return false;
  }

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  String url = remoteUrl(suffix);
  if (!httpBeginUrl(http, plainClient, secureClient, url)) {
    lastRemoteStatus = "http begin failed";
    return false;
  }

  if (strlen(remoteApiKey) > 0) http.addHeader("x-api-token", remoteApiKey);
  http.setTimeout(REMOTE_GET_TIMEOUT_MS);
  httpCodeOut = http.GET();
  if (httpCodeOut > 0) responseOut = http.getString();
  http.end();

  lastRemoteStatus = "GET " + suffix + " -> " + String(httpCodeOut);
  return httpCodeOut >= 200 && httpCodeOut < 300;
}


void addRelayStateArray(JsonDocument& doc) {
  JsonArray relays = doc.createNestedArray("relays");
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    JsonObject r = relays.createNestedObject();
    r["channel"] = i + 1;
    r["state"] = relayState[i] ? "on" : "off";
    r["role"] = (i == MASTER_RELAY_INDEX) ? "master_valve_spigots" : "zone";
    if (i < ZONE_COUNT) r["zone"] = i + 1;
  }
}


void addScheduleArray(JsonDocument& doc) {
  JsonArray schedules = doc.createNestedArray("schedules");
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& d = dailySchedules[i];
    if (d.zoneIndex >= ZONE_COUNT) continue;

    JsonObject s = schedules.createNestedObject();
    s["id"] = i;
    s["channel"] = d.zoneIndex + 1;
    s["zone"] = zones[d.zoneIndex].name;
    s["enabled"] = d.enabled;
    s["startTime"] = two(d.startHour) + ":" + two(d.startMinute);
    s["durationSeconds"] = (uint32_t)d.runMinutes * 60UL;
  }
}


void addCurrentRunObject(JsonDocument& doc) {
  JsonObject run = doc.createNestedObject("currentRun");
  uint8_t count = activeZoneRunCount();
  run["active"] = count > 0;
  run["activeZoneCount"] = count;

  JsonArray runs = doc.createNestedArray("zoneRuns");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    JsonObject zr = runs.createNestedObject();
    zr["channel"] = i + 1;
    zr["zone"] = i + 1;
    zr["active"] = zoneRuns[i].active;
    zr["relayOn"] = relayState[i];

    if (zoneRuns[i].active) {
      uint32_t elapsed = millis() - zoneRuns[i].startedMs;
      uint32_t remainingMs = zoneRuns[i].durationMs > elapsed ? zoneRuns[i].durationMs - elapsed : 0;
      zr["remainingSeconds"] = (remainingMs + 999UL) / 1000UL;
      zr["remainingMs"] = remainingMs;
      zr["durationMs"] = zoneRuns[i].durationMs;
      zr["elapsedMs"] = elapsed;
      zr["manual"] = zoneRuns[i].manual;
    }
  }

  JsonObject spigots = doc.createNestedObject("spigotRun");
  spigots["active"] = spigotRun.active;
  spigots["masterValveChannel"] = MASTER_RELAY_CHANNEL;
  if (spigotRun.active) {
    uint32_t elapsed = millis() - spigotRun.startedMs;
    uint32_t remainingMs = spigotRun.durationMs > elapsed ? spigotRun.durationMs - elapsed : 0;
    spigots["remainingSeconds"] = (remainingMs + 999UL) / 1000UL;
    spigots["remainingMs"] = remainingMs;
  }

  doc["masterValveOn"] = relayState[MASTER_RELAY_INDEX];
  doc["masterValveChannel"] = MASTER_RELAY_CHANNEL;
}



bool ackRemoteCommand(const char* commandId, const char* status) {
  if (!remoteReady() || !commandId || strlen(commandId) == 0) return false;

  StaticJsonDocument<256> doc;
  const char* normalizedStatus = (status && strcmp(status, "failed") == 0) ? "failed" : "applied";
  doc["status"] = normalizedStatus;

  String body, response;
  int code;
  serializeJson(doc, body);

  String path = "/api/microcontroller/commands/";
  path += commandId;
  path += "/ack";

  bool ok = remotePostJson(path, body, response, code);
  if (ok) {
    strlcpy(lastCommandId, commandId, sizeof(lastCommandId));
    strlcpy(lastCommandStatus, normalizedStatus, sizeof(lastCommandStatus));
  }
  return ok;
}


void addTargetLocationObject(JsonDocument& doc) {
  JsonObject location = doc.createNestedObject("targetLocation");
  location["lat"] = gardenLatitude;
  location["lon"] = gardenLongitude;
  location["label"] = "garden";
}

void addDeviceSensorData(JsonDocument& doc) {
  JsonArray sensors = doc.createNestedArray("sensorData");

  JsonObject wifi = sensors.createNestedObject();
  wifi["source"] = "relay-hardware";
  wifi["type"] = "wifi_rssi";
  wifi["value"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  wifi["unit"] = "dBm";
  wifi["observedAtEpoch"] = (uint32_t)time(nullptr);

  JsonObject uptime = sensors.createNestedObject();
  uptime["source"] = "relay-hardware";
  uptime["type"] = "uptime";
  uptime["value"] = (uint32_t)(millis() / 1000UL);
  uptime["unit"] = "s";
  uptime["observedAtEpoch"] = (uint32_t)time(nullptr);

  JsonObject heap = sensors.createNestedObject();
  heap["source"] = "relay-hardware";
  heap["type"] = "free_heap";
  heap["value"] = ESP.getFreeHeap();
  heap["unit"] = "bytes";
  heap["observedAtEpoch"] = (uint32_t)time(nullptr);

  JsonObject weatherStub = sensors.createNestedObject();
  weatherStub["source"] = "relay-hardware";
  weatherStub["type"] = "weather_sensor_placeholder";
  weatherStub["observedAtEpoch"] = (uint32_t)time(nullptr);
  weatherStub["temperatureC"].set(nullptr);
  weatherStub["relativeHumidityPct"].set(nullptr);
  weatherStub["precipitationMm"].set(nullptr);
  weatherStub["solarIrradianceWm2"].set(nullptr);
  weatherStub["windSpeedMps"].set(nullptr);
  weatherStub["note"] = "No local weather sensors installed yet; schema reserved for future hardware sensors.";
}

void publishSensorDataNow() {
  if (!remoteReady()) return;

  StaticJsonDocument<1536> doc;
  doc["deviceId"] = remoteDeviceId;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  addTargetLocationObject(doc);
  addDeviceSensorData(doc);

  String body, response;
  int code;
  serializeJson(doc, body);
  remotePostJson("/api/microcontroller/sensors", body, response, code);
}

// PR #20 server contract:
 // POST /api/microcontroller/state preserves epoch, localTime, localDate,
 // homeWifiConnected, homeIp, targetLocation, sensorData, currentRun,
 // relays, schedules, firmwareVersion, clockValid, and lastCommandId.
void publishFullStateNow() {
  if (!remoteReady()) return;

  StaticJsonDocument<4096> doc;
  doc["deviceId"] = remoteDeviceId;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["clockValid"] = clockIsValid();
  doc["epoch"] = (uint32_t)time(nullptr);
  doc["localTime"] = clockString();
  doc["localDate"] = dateString();
  doc["timeZone"] = gardenTimeZone;
  doc["posixTimeZone"] = gardenPosixTimeZone;
  doc["homeWifiConnected"] = WiFi.status() == WL_CONNECTED;
  doc["homeIp"] = WiFi.localIP().toString();
  doc["lastCommandId"] = lastCommandId;

  addTargetLocationObject(doc);
  addDeviceSensorData(doc);

  addRelayStateArray(doc);
  addScheduleArray(doc);
  addCurrentRunObject(doc);

  String body, response;
  int code;
  serializeJson(doc, body);
  remotePostJson("/api/microcontroller/state", body, response, code);
}

void publishRelayStateNow() {
  if (!remoteReady()) return;

  StaticJsonDocument<1024> doc;
  addRelayStateArray(doc);

  String body, response;
  int code;
  serializeJson(doc, body);
  remotePostJson("/api/microcontroller/relays/state", body, response, code);
}


void publishSchedulesNow() {
  if (!remoteReady()) return;

  StaticJsonDocument<2048> doc;
  addScheduleArray(doc);

  String body, response;
  int code;
  serializeJson(doc, body);
  remotePostJson("/api/microcontroller/schedules", body, response, code);
}


bool parseStartTimeToZone(const char* startTime, uint8_t& hourOut, uint8_t& minuteOut) {
  if (!startTime) return false;
  String s = String(startTime);
  s.trim();
  int colon = s.indexOf(':');
  if (colon <= 0) return false;

  int h = s.substring(0, colon).toInt();
  int m = s.substring(colon + 1).toInt();

  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  hourOut = (uint8_t)h;
  minuteOut = (uint8_t)m;
  return true;
}

void applyScheduleArray(JsonArray schedules) {
  if (schedules.isNull()) return;

  clearDailySchedules();

  for (JsonObject item : schedules) {
    if (dailyScheduleCount >= MAX_DAILY_SCHEDULES) break;

    int zoneNumber = item["channel"] | 0; // API channel == user-visible zone number
    if (zoneNumber < 1 || zoneNumber > ZONE_COUNT) continue;

    uint8_t zoneIndex = zoneNumber - 1;

    if (item["zone"].is<const char*>()) {
      strlcpy(zones[zoneIndex].name, item["zone"], sizeof(zones[zoneIndex].name));
    }

    uint8_t h = 0;
    uint8_t m = 0;
    if (!parseStartTimeToZone(item["startTime"] | "", h, m)) continue;

    int seconds = item["durationSeconds"] | 0;
    int runMinutes = constrain((seconds + 59) / 60, 1, 240);
    bool enabled = item["enabled"].is<bool>() ? (bool)item["enabled"] : true;

    addDailySchedule(zoneIndex, h, m, (uint16_t)runMinutes, enabled);
  }

  saveConfig();
  publishSchedulesNow();
}


void applyRemoteCommand(JsonObject cmd) {
  if (!cmd["command"].is<JsonObject>()) return;
  JsonObject nested = cmd["command"].as<JsonObject>();

  const char* commandId = nested["id"] | "";
  bool applied = false;

  String type = String((const char*)(nested["type"] | ""));
  if (type == "location_update" || type == "config_update") {
    if (nested["targetLocation"].is<JsonObject>()) {
      JsonObject loc = nested["targetLocation"].as<JsonObject>();
      if (loc["lat"].is<double>()) gardenLatitude = loc["lat"];
      if (loc["lon"].is<double>()) gardenLongitude = loc["lon"];
    }
    if (nested["timeZone"].is<const char*>()) strlcpy(gardenTimeZone, nested["timeZone"], sizeof(gardenTimeZone));
    if (nested["posixTimeZone"].is<const char*>()) strlcpy(gardenPosixTimeZone, nested["posixTimeZone"], sizeof(gardenPosixTimeZone));
    setupGardenTimeZone();
    saveConfig();
    lastRemoteStatus = "applied " + type;
    applied = true;
    ackRemoteCommand(commandId, "applied");
    publishFullStateNow();
    return;
  }

  if (type == "schedule_update") {
    if (nested["targetLocation"].is<JsonObject>()) {
      JsonObject loc = nested["targetLocation"].as<JsonObject>();
      if (loc["lat"].is<double>()) gardenLatitude = loc["lat"];
      if (loc["lon"].is<double>()) gardenLongitude = loc["lon"];
    }
    if (nested["timeZone"].is<const char*>()) strlcpy(gardenTimeZone, nested["timeZone"], sizeof(gardenTimeZone));
    if (nested["posixTimeZone"].is<const char*>()) strlcpy(gardenPosixTimeZone, nested["posixTimeZone"], sizeof(gardenPosixTimeZone));
    setupGardenTimeZone();

    JsonArray schedules = nested["schedules"].as<JsonArray>();
    if (!schedules.isNull()) {
      applyScheduleArray(schedules);
      lastRemoteStatus = "applied schedule_update";
      applied = true;
    } else {
      saveConfig();
    }
    ackRemoteCommand(commandId, applied ? "applied" : "failed");
    publishFullStateNow();
    return;
  }

  int zoneNumber = nested["channel"] | 0; // API channel == user-visible zone number for 1-5, channel 6 is master/spigots
  String action = String((const char*)(nested["action"] | ""));

  if (zoneNumber >= 1 && zoneNumber <= ZONE_COUNT) {
    uint8_t zoneIndex = zoneNumber - 1;
    uint32_t durationSeconds = commandDurationSeconds(nested);

    if (action == "toggle") {
      if (relayState[zoneIndex]) stopZone(zoneIndex);
      else startRunSeconds(zoneIndex, durationSeconds, true);
      applied = true;
    } else if (action == "on") {
      // PR #22 server contract: zone ON commands are timed runs and include
      // durationSeconds, defaulting server-side to 900 seconds.
      startRunSeconds(zoneIndex, durationSeconds, true);
      applied = true;
    } else if (action == "off") {
      stopZone(zoneIndex);
      applied = true;
    }
  } else if (zoneNumber == MASTER_RELAY_CHANNEL) {
    uint32_t durationSeconds = commandDurationSeconds(nested);

    if (action == "toggle") {
      if (spigotRun.active) stopSpigotRun();
      else startSpigotRunSeconds(durationSeconds);
      applied = true;
    } else if (action == "on") {
      startSpigotRunSeconds(durationSeconds);
      applied = true;
    } else if (action == "off") {
      stopSpigotRun();
      applied = true;
    }
  }

  ackRemoteCommand(commandId, applied ? "applied" : "failed");
  publishRelayStateNow();
  publishFullStateNow();
}


void remotePollCommands() {
  if (!remoteReady()) return;

  String response;
  int code;
  bool ok = remoteGetJson("/api/queue/next?wait=" + String(REMOTE_LONG_POLL_WAIT_SECONDS), response, code);
  if (code == 204) {
    lastRemoteStatus = "GET /api/queue/next -> 204 empty";
    return;
  }

  if (!ok || response.length() == 0) return;

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    lastRemoteStatus = "queue JSON parse failed";
    return;
  }

  if (doc.is<JsonObject>()) applyRemoteCommand(doc.as<JsonObject>());
}

void serviceRemoteApi() {
  if (!remoteReady()) return;

  uint32_t nowMs = millis();

  // State telemetry lane: periodic heartbeat/reporting.
  if (nowMs - lastRemoteRelayPublishMs > REMOTE_TELEMETRY_INTERVAL_MS) {
    lastRemoteRelayPublishMs = nowMs;
    publishFullStateNow();
  }

  // Sensor lane: slower device/sensor observations.
  if (nowMs - lastRemoteSensorPublishMs > REMOTE_SENSOR_INTERVAL_MS) {
    lastRemoteSensorPublishMs = nowMs;
    publishSensorDataNow();
  }

  // Command lane: continuous long-poll. Do not throttle this with
  // remoteIntervalSeconds; the server holds /api/queue/next?wait=25 and
  // wakes immediately when a command is queued.
  lastRemoteCommandPollMs = nowMs;
  remotePollCommands();
}




void buildStateJson(JsonDocument& doc) {
  doc["deviceId"] = remoteDeviceId;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["lastCommandId"] = lastCommandId;
  doc["lastCommandStatus"] = lastCommandStatus;
  doc["title"] = "Garden Relay 5 + Master";
  doc["zoneCount"] = ZONE_COUNT;
  doc["relayCount"] = RELAY_COUNT;
  doc["masterValveChannel"] = MASTER_RELAY_CHANNEL;
  doc["masterValveOn"] = relayState[MASTER_RELAY_INDEX];
  doc["spigotActive"] = spigotRun.active;
  doc["date"] = dateString();
  doc["time"] = clockString();
  doc["clockValid"] = clockIsValid();
  doc["epoch"] = (uint32_t)time(nullptr);
  doc["apSsid"] = apSsid;
  doc["apPass"] = apPass;
  doc["staSsid"] = staSsid;
  doc["staPass"] = staPass;
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["homeWifiConnected"] = WiFi.status() == WL_CONNECTED;
  doc["homeIp"] = WiFi.localIP().toString();
  doc["homeWifiStatus"] = lastStaStatus;
  doc["remoteEnabled"] = remoteEnabled;
  doc["remoteApiBase"] = remoteApiBase;
  doc["remoteDeviceId"] = remoteDeviceId;
  doc["remoteApiKey"] = remoteApiKey;
  doc["remoteStatus"] = lastRemoteStatus;
  doc["longPollWaitSeconds"] = REMOTE_LONG_POLL_WAIT_SECONDS;
  doc["remoteGetTimeoutMs"] = REMOTE_GET_TIMEOUT_MS;
  doc["telemetryIntervalMs"] = REMOTE_TELEMETRY_INTERVAL_MS;
  doc["sensorIntervalMs"] = REMOTE_SENSOR_INTERVAL_MS;
  doc["commandPollingMode"] = "continuous_long_poll";
  doc["remoteIntervalSecondsLegacy"] = remoteIntervalSeconds;
  doc["gardenLatitude"] = gardenLatitude;
  doc["gardenLongitude"] = gardenLongitude;
  doc["gardenTimeZone"] = gardenTimeZone;
  doc["gardenPosixTimeZone"] = gardenPosixTimeZone;

  JsonArray z = doc.createNestedArray("zones");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    JsonObject o = z.createNestedObject();
    o["zone"] = i + 1;
    o["name"] = zones[i].name;
    o["enabled"] = zones[i].enabled;
    o["relayOn"] = relayState[i];
    JsonObject color = o.createNestedObject("color");
    color["r"] = ZONE_COLORS[i].r;
    color["g"] = ZONE_COLORS[i].g;
    color["b"] = ZONE_COLORS[i].b;
  }
  JsonArray zoneColors = doc.createNestedArray("zoneColors");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    JsonObject c = zoneColors.createNestedObject();
    c["zone"] = i + 1;
    c["channel"] = i + 1;
    c["r"] = ZONE_COLORS[i].r;
    c["g"] = ZONE_COLORS[i].g;
    c["b"] = ZONE_COLORS[i].b;
  }

  JsonArray sched = doc.createNestedArray("dailySchedules");
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& d = dailySchedules[i];
    if (d.zoneIndex >= ZONE_COUNT) continue;
    JsonObject s = sched.createNestedObject();
    s["id"] = i;
    s["zone"] = d.zoneIndex + 1;
    s["zoneName"] = zones[d.zoneIndex].name;
    s["enabled"] = d.enabled;
    s["startHour"] = d.startHour;
    s["startMinute"] = d.startMinute;
    s["start"] = compactTime(d.startHour, d.startMinute);
    s["timeValue"] = htmlTimeValue(d.startHour, d.startMinute);
    s["runMinutes"] = d.runMinutes;
  }
  doc["dailyScheduleCount"] = dailyScheduleCount;
  doc["maxDailySchedules"] = MAX_DAILY_SCHEDULES;

  JsonObject run = doc.createNestedObject("currentRun");
  uint8_t activeCount = activeZoneRunCount();
  run["active"] = activeCount > 0;
  run["activeZoneCount"] = activeCount;

  JsonArray zruns = doc.createNestedArray("zoneRuns");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    JsonObject zr = zruns.createNestedObject();
    zr["zone"] = i + 1;
    zr["channel"] = i + 1;
    zr["active"] = zoneRuns[i].active;
    zr["relayOn"] = relayState[i];

    if (zoneRuns[i].active) {
      uint32_t elapsed = millis() - zoneRuns[i].startedMs;
      uint32_t remaining = zoneRuns[i].durationMs > elapsed ? zoneRuns[i].durationMs - elapsed : 0;
      zr["durationMs"] = zoneRuns[i].durationMs;
      zr["elapsedMs"] = elapsed;
      zr["remainingMs"] = remaining;
      zr["pct"] = zoneRuns[i].durationMs ? min(1.0f, (float)elapsed / (float)zoneRuns[i].durationMs) : 0.0f;
      zr["manual"] = zoneRuns[i].manual;
    }
  }

  JsonObject spigots = doc.createNestedObject("spigotRun");
  spigots["active"] = spigotRun.active;
  spigots["masterValveChannel"] = MASTER_RELAY_CHANNEL;
  if (spigotRun.active) {
    uint32_t elapsed = millis() - spigotRun.startedMs;
    uint32_t remaining = spigotRun.durationMs > elapsed ? spigotRun.durationMs - elapsed : 0;
    spigots["durationMs"] = spigotRun.durationMs;
    spigots["elapsedMs"] = elapsed;
    spigots["remainingMs"] = remaining;
    spigots["pct"] = spigotRun.durationMs ? min(1.0f, (float)elapsed / (float)spigotRun.durationMs) : 0.0f;
  }
}

void sendStateJson() {
  StaticJsonDocument<4096> doc;
  doc["ok"] = true;
  buildStateJson(doc);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetTime() {
  if (!server.hasArg("epoch")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing epoch\"}");
    return;
  }
  time_t epoch = (time_t)server.arg("epoch").toInt();
  timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  setupGardenTimeZone();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRelay() {
  if (!server.hasArg("zone") || !server.hasArg("state")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing zone/state\"}");
    return;
  }
  int zone = server.arg("zone").toInt();
  int state = server.arg("state").toInt();
  if (zone < 1 || zone > ZONE_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"zone out of range; zones are 1-5, relay 6 is master/spigots\"}");
    return;
  }
  setZoneRelay((uint8_t)(zone - 1), state == 1);
  publishRelayStateNow();
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}


void handleManualRun() {
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (minutes < 1 || minutes > 240) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad minutes\"}");
    return;
  }

  if (zone == MASTER_RELAY_CHANNEL) {
    startSpigotRun((uint16_t)minutes);
    publishRelayStateNow();
    publishFullStateNow();
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"spigots\"}");
    return;
  }

  if (zone < 1 || zone > ZONE_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad zone; zones are 1-5 or 6 for spigots\"}");
    return;
  }

  startRun((uint8_t)(zone - 1), (uint16_t)minutes, true);
  publishRelayStateNow();
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}


void handleSpigotRun() {
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : DEFAULT_SPIGOT_RUN_MINUTES;
  String action = server.hasArg("action") ? server.arg("action") : "on";

  if (action == "off" || minutes == 0) {
    stopSpigotRun();
  } else {
    minutes = constrain(minutes, 1, 240);
    startSpigotRun((uint16_t)minutes);
  }

  publishRelayStateNow();
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleScheduleAdd() {
  int zone = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  const char* timeText = server.hasArg("time") ? server.arg("time").c_str() : "";

  uint8_t h = 0;
  uint8_t m = 0;
  if (zone < 1 || zone > ZONE_COUNT || minutes < 1 || minutes > 240 || !parseStartTimeToZone(timeText, h, m)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad schedule\"}");
    return;
  }

  if (!addDailySchedule((uint8_t)(zone - 1), h, m, (uint16_t)minutes, true)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"schedule limit reached\"}");
    return;
  }

  saveConfig();
  publishSchedulesNow();
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleScheduleDelete() {
  int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
  if (id < 0 || id >= dailyScheduleCount) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad id\"}");
    return;
  }

  for (uint8_t i = id; i + 1 < dailyScheduleCount; i++) {
    dailySchedules[i] = dailySchedules[i + 1];
  }
  dailyScheduleCount--;

  saveConfig();
  publishSchedulesNow();
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAllOff() {
  stopRun();
  publishRelayStateNow();
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleBuzzerTest() {
  chirp(2200, 70);
  delay(35);
  chirp(2500, 45);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleFactoryReset() {
  factoryReset();
  server.send(200, "text/plain", "Factory reset complete. Rebooting.");
  delay(500);
  ESP.restart();
}


void handleApiFeatures() {
  StaticJsonDocument<1024> doc;
  doc["ok"] = true;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["zoneCount"] = ZONE_COUNT;
  doc["relayCount"] = RELAY_COUNT;
  doc["masterValveChannel"] = MASTER_RELAY_CHANNEL;
  JsonArray telemetry = doc.createNestedArray("telemetry");
  telemetry.add("clock"); telemetry.add("wifi"); telemetry.add("relays"); telemetry.add("schedules"); telemetry.add("currentRun"); telemetry.add("spigotRun"); telemetry.add("sensorData"); telemetry.add("targetLocation");
  JsonArray controls = doc.createNestedArray("controls");
  controls.add("syncTime"); controls.add("manualRun"); controls.add("spigotRun"); controls.add("setRelay"); controls.add("allOff"); controls.add("buzzerTest"); controls.add("factoryReset");
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiConfigGet() {
  StaticJsonDocument<2048> doc;
  doc["ok"] = true;
  JsonObject cfg = doc.createNestedObject("config");
  cfg["apSsid"] = apSsid; cfg["apPass"] = apPass; cfg["staSsid"] = staSsid; cfg["staPass"] = staPass;
  cfg["remoteEnabled"] = remoteEnabled; cfg["remoteApiBase"] = remoteApiBase; cfg["remoteDeviceId"] = remoteDeviceId; cfg["remoteApiKey"] = remoteApiKey;
  cfg["remoteIntervalSeconds"] = remoteIntervalSeconds; cfg["gardenLatitude"] = gardenLatitude; cfg["gardenLongitude"] = gardenLongitude; cfg["gardenTimeZone"] = gardenTimeZone; cfg["gardenPosixTimeZone"] = gardenPosixTimeZone;
  JsonArray z = cfg.createNestedArray("zones");
  for (uint8_t i = 0; i < ZONE_COUNT; i++) { JsonObject o = z.createNestedObject(); o["zone"] = i + 1; o["enabled"] = zones[i].enabled; o["name"] = zones[i].name; }
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}

void handleApiConfigSet() {
  StaticJsonDocument<3072> doc;
  if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
  if (doc["apSsid"].is<const char*>()) strlcpy(apSsid, doc["apSsid"], sizeof(apSsid));
  if (doc["apPass"].is<const char*>()) { String p = doc["apPass"].as<String>(); if (p.length() == 0 || (p.length() >= 8 && p.length() <= 63)) strlcpy(apPass, p.c_str(), sizeof(apPass)); }
  if (doc["staSsid"].is<const char*>()) strlcpy(staSsid, doc["staSsid"], sizeof(staSsid));
  if (doc["staPass"].is<const char*>()) strlcpy(staPass, doc["staPass"], sizeof(staPass));
  if (doc["remoteEnabled"].is<bool>()) remoteEnabled = doc["remoteEnabled"];
  if (doc["remoteApiBase"].is<const char*>()) strlcpy(remoteApiBase, doc["remoteApiBase"], sizeof(remoteApiBase));
  if (doc["remoteDeviceId"].is<const char*>()) strlcpy(remoteDeviceId, doc["remoteDeviceId"], sizeof(remoteDeviceId));
  if (doc["remoteApiKey"].is<const char*>()) strlcpy(remoteApiKey, doc["remoteApiKey"], sizeof(remoteApiKey));
  if (doc["remoteIntervalSeconds"].is<int>()) remoteIntervalSeconds = constrain((int)doc["remoteIntervalSeconds"], 15, 3600);
  if (doc["gardenLatitude"].is<double>()) gardenLatitude = doc["gardenLatitude"];
  if (doc["gardenLongitude"].is<double>()) gardenLongitude = doc["gardenLongitude"];
  if (doc["gardenTimeZone"].is<const char*>()) strlcpy(gardenTimeZone, doc["gardenTimeZone"], sizeof(gardenTimeZone));
  if (doc["gardenPosixTimeZone"].is<const char*>()) strlcpy(gardenPosixTimeZone, doc["gardenPosixTimeZone"], sizeof(gardenPosixTimeZone));
  if (doc["zones"].is<JsonArray>()) { for (JsonObject z : doc["zones"].as<JsonArray>()) { int zone = z["zone"] | 0; if (zone < 1 || zone > ZONE_COUNT) continue; uint8_t i = zone - 1; if (z["enabled"].is<bool>()) zones[i].enabled = z["enabled"]; if (z["name"].is<const char*>()) strlcpy(zones[i].name, z["name"], sizeof(zones[i].name)); }}
  setupGardenTimeZone(); saveConfig(); publishSchedulesNow(); publishFullStateNow(); if (strlen(staSsid) > 0) connectSta(false);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiScheduleSet() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}"); return; }
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, server.arg("plain")) || !doc["schedules"].is<JsonArray>()) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
  applyScheduleArray(doc["schedules"].as<JsonArray>());
  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Garden Relay 5 + Master</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:Arial,Helvetica,sans-serif;background:#f4f1e8;color:#111}header{padding:14px;background:#223d2a;color:#fff}h1{font-size:22px;margin:0}.sub{font-size:13px;opacity:.85;margin-top:4px}main{padding:10px;max-width:900px;margin:auto}.card{background:#fff;border:1px solid #ddd;border-radius:14px;padding:12px;margin:10px 0;box-shadow:0 1px 5px #0001}.map{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.zone{aspect-ratio:1.5;border:3px solid #111;border-radius:8px;display:flex;flex-direction:column;align-items:center;justify-content:center;font-weight:800;font-size:22px;background:#fff}.zone small{font-size:12px;font-weight:600}.zone.on{background:#111;color:#fff}.btn{border:0;border-radius:9px;background:#31543a;color:#fff;padding:10px 12px;font-size:16px;text-decoration:none;text-align:center;margin:3px}.danger{background:#9a2b2b}.muted{color:#555;font-size:13px}.meter{height:26px;border:2px solid #111;border-radius:6px;overflow:hidden}.fill{height:100%;background:#111;width:0}.row{display:flex;justify-content:space-between;gap:10px;margin:4px 0;font-size:15px}.nav{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
</style></head>
<body>
<header><h1>Garden Relay 5 + Master</h1><div id="headSub" class="sub">loading</div><div class="nav"><a class="btn" href="/admin">Admin</a><button class="btn" onclick="syncTime()">Sync Phone Time</button><button class="btn danger" onclick="allOff()">All Off</button></div></header>
<main><section class="card"><h2>Zone Map</h2><div id="map" class="map"></div></section><section class="card"><h2>Current Run</h2><div id="runText" class="muted">Idle</div><div class="meter"><div id="runFill" class="fill"></div></div></section><section class="card"><h2>Spigots / Master Valve</h2><div id="spigotText" class="muted">loading</div><button class="btn" onclick="spigots()">Run Spigots</button><button class="btn danger" onclick="api('/api/spigots-run?action=off')">Stop Spigots</button></section><section class="card"><h2>Schedule</h2><div id="schedule"></div></section></main>
<script>
function fmtRemain(ms){ms=Math.max(0,ms||0);return Math.floor(ms/60000)+'m '+(Math.floor(ms/1000)%60)+'s'}
async function api(url,opt){await fetch(url,Object.assign({headers:{'Content-Type':'application/json'}},opt||{})); refresh()}
async function allOff(){await api('/api/alloff')}
async function syncTime(){await api('/api/time/set?epoch='+Math.floor(Date.now()/1000))}
async function manual(zone){const m=prompt('Run Zone '+zone+' for how many minutes?','15');if(m)await api('/api/manual-run?zone='+zone+'&minutes='+encodeURIComponent(m))}
async function spigots(){const m=prompt('Turn spigots on for how many minutes?','15');if(m)await api('/api/spigots-run?minutes='+encodeURIComponent(m))}
async function refresh(){
 const r=await fetch('/api/state',{cache:'no-store'}); const s=await r.json();
 headSub.textContent=(s.clockValid?s.date+' '+s.time+' '+(s.gardenTimeZone||'garden time'):'Clock not set')+' | AP '+s.apSsid+' '+s.apIp+' | Home WiFi '+(s.homeWifiConnected?s.homeIp:(s.homeWifiStatus||'off'))+' | Remote '+(s.remoteEnabled?s.remoteStatus:'disabled');
 map.innerHTML=(s.zones||[]).map(z=>`<div class="zone ${z.relayOn?'on':''}" onclick="manual(${z.zone})">Zone ${z.zone}<small>${z.name||''}</small><small>${z.relayOn?'ON':'tap to run'}</small></div>`).join('');
 schedule.innerHTML=(s.dailySchedules||[]).map(d=>`<div class=row><b>Zone ${d.zone}</b><span>${d.enabled?'On':'Off'} · ${d.start} · ${d.runMinutes} min</span></div>`).join('') || '<div class="muted">No schedules configured</div>';
 if(typeof spigotText!=='undefined'){const sp=s.spigotRun||{};spigotText.textContent='Master valve relay '+(s.masterValveChannel||6)+': '+(s.masterValveOn?'ON':'OFF')+' · Spigots '+(sp.active?('ON, '+fmtRemain(sp.remainingMs)+' remaining'):'OFF');}
 const run=s.currentRun||{};
 const activeRuns=(s.zoneRuns||[]).filter(z=>z.active);
 if(run.active&&activeRuns.length){
   runText.textContent='Running '+activeRuns.map(z=>'Zone '+z.zone+' · '+fmtRemain(z.remainingMs)).join(' | ');
   const avg=activeRuns.reduce((sum,z)=>sum+(z.pct||0),0)/activeRuns.length;
   runFill.style.width=Math.round(avg*100)+'%';
 }
 else{runText.textContent='Idle'; runFill.style.width='0%';}
}
refresh();setInterval(refresh,2000);
</script></body></html>
)rawliteral";

String adminPage() {
  String h;
  h += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Garden Relay 5 + Master Admin</title><style>";
  h += "*{box-sizing:border-box}body{font-family:Arial,Helvetica,sans-serif;margin:0;background:#f4f1e8;color:#111}header{background:#223d2a;color:white;padding:14px}main{padding:10px;max-width:960px;margin:auto}.card{background:white;border:1px solid #ddd;border-radius:14px;padding:12px;margin:10px 0}label{font-weight:700;font-size:13px}input{width:100%;font-size:16px;padding:8px;margin:4px 0 10px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.zones{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}.btn,button{display:inline-block;border:0;border-radius:9px;background:#31543a;color:#fff;padding:10px 12px;font-size:16px;text-decoration:none;margin:3px}.danger{background:#9a2b2b}@media(max-width:700px){.grid{grid-template-columns:1fr}}</style></head><body>";
  h += "<header><h1>Garden Relay 5 + Master Admin</h1><a class='btn' href='/'>Map</a><button onclick=\"fetch('/api/time/set?epoch='+Math.floor(Date.now()/1000)).then(()=>alert('time synced'))\">Sync Phone Time</button><button class='danger' onclick=\"fetch('/api/alloff').then(()=>location.reload())\">All Off</button><button onclick=\"const m=prompt('Run spigots for how many minutes?','15'); if(m) fetch('/api/spigots-run?minutes='+encodeURIComponent(m)).then(()=>location.reload())\">Run Spigots</button><button onclick=\"fetch('/api/spigots-run?action=off').then(()=>location.reload())\">Stop Spigots</button></header><main>";
  h += "<form method='post' action='/admin/save'>";
  h += "<section class='card'><h2>WiFi</h2>";
  h += "<p><b>Access point:</b> " + String(apSsid) + " at " + WiFi.softAPIP().toString() + "</p>";
  h += "<p><b>Home WiFi:</b> " + String(WiFi.status() == WL_CONNECTED ? "connected at " + WiFi.localIP().toString() : lastStaStatus) + "</p>";
  h += "<div class='grid'>";
  h += "<label>AP SSID<input name='apSsid' value='" + htmlEscape(apSsid) + "'></label>";
  h += "<label>AP Password, 8-63 chars or blank open<input name='apPass' value='" + htmlEscape(apPass) + "'></label>";
  h += "<label>Home WiFi SSID<input name='staSsid' value='" + htmlEscape(staSsid) + "'></label>";
  h += "<label>Home WiFi Password<input name='staPass' value='" + htmlEscape(staPass) + "'></label>";
  h += "</div></section>";

  h += "<section class='card'><h2>Remote API</h2>";
  h += "<p><b>Status:</b> " + htmlEscape(lastRemoteStatus) + "</p>";
  h += "<label><input type='checkbox' name='remoteEnabled' " + String(remoteEnabled ? "checked" : "") + "> Enable remote API integration</label>";
  h += "<label>Remote API Base URL<input name='remoteApiBase' value='" + htmlEscape(remoteApiBase) + "'></label>";
  h += "<label>Device ID / local metadata<input name='remoteDeviceId' value='" + htmlEscape(remoteDeviceId) + "'></label>";
  h += "<label>API Token<input name='remoteApiKey' value='" + htmlEscape(remoteApiKey) + "'></label>";
  h += "<label>Legacy remote interval seconds <span style='font-weight:400'>(commands now use continuous long-poll; state telemetry is fixed at 15s)</span><input type='number' min='15' max='3600' name='remoteIntervalSeconds' value='" + String(remoteIntervalSeconds) + "'></label>";
  h += "<h3>Garden Location / Timezone</h3>";
  h += "<p>Schedule times run in this configured garden timezone. If the garden moves, update coordinates and timezone together. The server should resolve timezone from coordinates.</p>";
  h += "<label>Garden latitude<input name='gardenLatitude' value='" + String(gardenLatitude, 6) + "'></label>";
  h += "<label>Garden longitude<input name='gardenLongitude' value='" + String(gardenLongitude, 6) + "'></label>";
  h += "<label>Garden IANA timezone<input name='gardenTimeZone' value='" + htmlEscape(gardenTimeZone) + "'></label>";
  h += "<label>ESP32 POSIX timezone<input name='gardenPosixTimeZone' value='" + htmlEscape(gardenPosixTimeZone) + "'></label>";
  h += "<button type='button' onclick=\"fetch('/api/remote/test').then(r=>r.json()).then(j=>alert(JSON.stringify(j,null,2)))\">Test Remote API</button>";
  h += "</section>";

  h += "<section class='card'><h2>Zones</h2><p>There are 5 valve zones. Relay channel 6 is reserved for the master valve/spigots and is not scheduled as a zone.</p><div class='zones'>";
  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    String p = "z" + String(i + 1);
    h += "<div class='card'><h3>Zone " + String(i + 1) + "</h3>";
    h += "<label><input type='checkbox' name='" + p + "enabled' " + String(zones[i].enabled ? "checked" : "") + "> Enabled</label>";
    h += "<label>Name<input name='" + p + "name' value='" + htmlEscape(zones[i].name) + "'></label>";
    h += "<div>";
    h += "<button type='button' onclick=\"fetch('/api/relay?zone=" + String(i + 1) + "&state=1').then(()=>location.reload())\">Turn On</button>";
    h += "<button type='button' onclick=\"fetch('/api/relay?zone=" + String(i + 1) + "&state=0').then(()=>location.reload())\">Turn Off</button>";
    h += "<button type='button' onclick=\"const m=prompt('Run Zone " + String(i + 1) + " for how many minutes?','15'); if(m) fetch('/api/manual-run?zone=" + String(i + 1) + "&minutes='+encodeURIComponent(m)).then(()=>location.reload())\">Run Timed</button>";
    h += "</div>";
    h += "</div>";
  }
  h += "</div></section>";

  h += "<section class='card'><h2>Daily Schedules</h2><p>Add as many daily schedule entries as needed, up to " + String(MAX_DAILY_SCHEDULES) + " entries stored on the relay. Each line format: <b>zone,HH:MM,minutes,on/off</b>. Example: <code>1,06:00,10,on</code></p>";
  h += "<textarea name='schedulesText' rows='12' style='width:100%;font-family:monospace;font-size:15px'>";
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    h += htmlEscape(scheduleLine(i));
    h += "\n";
  }
  h += "</textarea></section><button>Save Settings</button></form>";
  h += "<section class='card'><h2>API Schedule Manager</h2><p>Uses firmware API directly to set all schedule rows.</p><div id='apiSchedRows'></div><button type='button' onclick='addApiScheduleRow()'>Add Row</button><button type='button' onclick='applyApiSchedules()'>Apply Schedules via API</button></section>";
  h += "<script>";
  h += "async function loadApiState(){const r=await fetch('/api/state',{cache:'no-store'});return r.json()}";
  h += "function addApiScheduleRow(v){const c=document.getElementById('apiSchedRows');const d=document.createElement('div');d.className='grid';d.innerHTML=`<input placeholder=Zone min=1 max=5 type=number value='${(v&&v.zone)||1}'/><input placeholder=HH:MM value='${(v&&v.timeValue)||\"06:00\"}'/><input placeholder=Minutes min=1 max=240 type=number value='${(v&&v.runMinutes)||10}'/><select><option value='on'>on</option><option value='off'>off</option></select><button type=button onclick='this.parentElement.remove()'>Delete</button>`;if(v&&v.enabled===false){d.querySelector('select').value='off';}c.appendChild(d)}";
  h += "async function seedApiRows(){const s=await loadApiState();(s.dailySchedules||[]).forEach(addApiScheduleRow)}";
  h += "async function applyApiSchedules(){const rows=[...document.querySelectorAll('#apiSchedRows .grid')];const schedules=rows.map(r=>({channel:Number(r.children[0].value),startTime:r.children[1].value,durationSeconds:Number(r.children[2].value)*60,enabled:r.children[3].value==='on'}));const res=await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({schedules})});if(!res.ok){alert('Failed to apply schedules');return;}alert('Schedules updated');location.reload()}";
  h += "seedApiRows();";
  h += "</script>";
  h += "<section class='card'><h2>Reset</h2><button class='danger' onclick=\"if(confirm('Factory reset settings?'))fetch('/api/factory-reset').then(r=>r.text()).then(alert)\">Factory Reset</button></section>";
  h += "</main></body></html>";
  return h;
}

void handleRoot() {
  server.send(200, "text/html", FPSTR(MAIN_PAGE));
}

const char ADMIN_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Garden Relay Admin</title>
<style>
body{font-family:Inter,Arial,sans-serif;margin:0;background:#f1f5f9;color:#123}.shell{max-width:1200px;margin:0 auto;padding:12px}.panel{background:#fff;border:1px solid #dbe7ef;border-radius:16px;padding:14px;margin-bottom:12px}
.layout{display:grid;grid-template-columns:1fr 1.4fr;gap:12px}.relay-grid{list-style:none;padding:0;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.relay-card{border:1px solid #d8e3eb;border-radius:12px;padding:10px}
.zone{stroke-width:2}.zone-active{filter:saturate(1.2)} .status-mismatch{color:#b42345} .status-synced{color:#18794e}
.timeline{display:grid;gap:8px}.timeline-row{display:grid;grid-template-columns:90px 1fr;gap:8px}.timeline-track{height:26px;background:#f3f7fb;border:1px solid #d8e3eb;border-radius:999px;position:relative}.timeline-block{position:absolute;top:2px;height:20px;border-radius:999px;color:#fff;font-size:.75rem;padding:0 8px;display:flex;align-items:center;white-space:nowrap}
button,input,select{border:1px solid #c7d8e5;border-radius:8px;padding:7px} label{display:flex;flex-direction:column;gap:6px;font-size:.92rem} .grid{display:grid;grid-template-columns:repeat(2,minmax(220px,1fr));gap:10px 12px;margin:10px 0} .actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap} .header-actions button{flex:1 1 170px} .schedule-actions{margin-top:10px} .danger{background:#9a2b2b;color:#fff;border-color:#7d2020}
@media(max-width:900px){.layout{grid-template-columns:1fr}.relay-grid{grid-template-columns:1fr}.grid{grid-template-columns:1fr}}
</style></head><body><div class="shell">
<section class="panel"><h1>Castle Hills Garden Manager (Firmware Local)</h1><p id="meta">Loading state...</p><p><a href="/">Mobile View</a></p><div class="actions header-actions"><button onclick="syncTime()">Sync Phone Time</button><button onclick="cmd('/api/alloff')">Turn Off All Relays</button></div></section>
<div class="layout"><section class="panel"><h2>Garden Zone Map</h2><svg viewBox="0 0 295.743 295.482"><polygon id="zone-4b" data-zone="4" class="zone" points="127.534,159.189 124.128,239.478 15.618,232.055 22.68,152.017 127.534,159.189"/><polygon id="zone-4a" data-zone="4" class="zone" points="146.876,95.99 134.762,166.15 198.935,169.661 205.581,96.842 146.876,95.99"/><polygon id="zone-1" data-zone="1" class="zone" points="205.581,96.842 264.287,97.694 263.108,173.173 198.935,169.661 205.581,96.842"/><polygon id="zone-5" data-zone="5" class="zone" points="32,46.388 46.493,52.635 95.676,139.247 128.342,140.128 127.534,159.189 22.68,152.017 32,46.388"/><polygon id="zone-3" data-zone="3" class="zone" points="152.903,89.893 152.691,13.195 50.949,12.06 136.836,89.869 152.903,89.893"/><polygon id="zone-2" data-zone="2" class="zone" points="249.304,90.041 249.095,14.271 152.691,13.195 152.903,89.893 249.304,90.041"/></svg></section>
<section class="panel"><h2>Zones (scheduled irrigation)</h2><ul id="relay-grid" class="relay-grid"></ul><h3>Schedule Timeline</h3><div id="timeline"></div><h3>Schedule Manager</h3><p>Create, update, and delete schedule rows directly in firmware.</p><div id="adminSchedRows"></div><div class="actions schedule-actions"><button onclick="addScheduleRow()">Add Schedule</button><button onclick="saveSchedules()">Save Schedules</button></div></section></div>
<section class="panel"><h2>WiFi Settings</h2><div class="grid"><label>AP SSID<input id="apSsid" autocomplete="off"></label><label>AP Password<input id="apPass" autocomplete="off"></label><label>Home WiFi SSID<input id="staSsid" autocomplete="off"></label><label>Home WiFi Password<input id="staPass" autocomplete="off"></label></div><div class="actions"><button onclick="saveConfig()">Save WiFi Settings</button></div></section>
<section class="panel"><h2>Remote API Settings</h2><div class="grid"><label>Remote API Base URL<input id="remoteApiBase" autocomplete="off"></label><label>Device ID / local metadata<input id="remoteDeviceId" autocomplete="off"></label><label>API Token<input id="remoteApiKey" autocomplete="off"></label><label><span>Remote API enabled</span><input type="checkbox" id="remoteEnabled"></label></div><div class="actions"><button onclick="testRemote()">Test Remote API</button></div></section>
<section class="panel"><h2>Garden Location & Time Zone</h2><div class="grid"><label>Garden latitude<input id="gardenLatitude" type="number" step="0.000001"></label><label>Garden longitude<input id="gardenLongitude" type="number" step="0.000001"></label><label>Garden IANA timezone<input id="gardenTimeZone" autocomplete="off"></label><label>ESP32 POSIX timezone<input id="gardenPosixTimeZone" autocomplete="off"></label></div><div class="actions"><button onclick="saveConfig()">Save Device Settings</button></div></section>
<section class="panel"><h2>Factory Reset</h2><p>Clear saved settings and reboot this device.</p><div class="actions"><button class="danger" onclick="factoryReset()">Factory Reset</button></div></section>
</div>
<script>
function zids(ch){if(ch===4)return ['zone-4a','zone-4b'];return ['zone-'+ch]}
function zoneChannel(z){return Number(z&&((z.channel??z.zone))||0)}
function colorByChannel(zoneColors, ch){return (zoneColors||[]).find(c=>Number(c.channel)===Number(ch))}
function toRgb(c){if(!c||!c.rgb)return '0,0,0';return `${Number(c.rgb.r)||0},${Number(c.rgb.g)||0},${Number(c.rgb.b)||0}`}
function buildTimelineRows(s){const m={};(s||[]).forEach(x=>{const k=`${x.channel}:${x.zone}`;(m[k]=m[k]||{zone:x.zone,channel:x.channel,schedules:[]}).schedules.push(x)});return Object.values(m)}
function renderTimeline(rows, zoneColors){if(!rows.length)return '<p>No schedules configured.</p>';return `<div class=timeline>${rows.map(r=>{const b=r.schedules.map(s=>{const p=String(s.startTime||'00:00').split(':');const l=((Number(p[0])*60+Number(p[1]))/1440)*100;const w=Math.max(2,Math.min(((Number(s.durationSeconds)||60)/86400)*100,100-l));const rgb=toRgb(colorByChannel(zoneColors, Number(r.channel||0)));return `<span class="timeline-block" style="left:${l}%;width:${w}%;background:rgb(${rgb})">${s.startTime} · ${Math.max(1,Math.round((Number(s.durationSeconds)||0)/60))} min</span>`}).join('');return `<div class=timeline-row><span>Zone ${r.channel}</span><div class=timeline-track>${b}</div></div>`}).join('')}</div>`}
function addScheduleRow(v){const rows=document.getElementById('adminSchedRows');const row=document.createElement('div');row.className='actions';const z=Number(v&&v.zone||1);const t=(v&&v.timeValue)||'06:00';const m=Number(v&&v.runMinutes||10);const en=v&&v.enabled===false?'off':'on';row.innerHTML=`<input type=number min=1 max=5 value='${z}' title='Zone'><input type=time style='max-width:70px' value='${t}' title='Start'><input type=number min=1 max=240 style='max-width:52px' value='${m}' title='Minutes'><select title='Enabled'><option value='on'>On</option><option value='off'>Off</option></select><button class='danger' onclick="this.parentElement.remove()">Delete</button>`;row.querySelector('select').value=en;rows.appendChild(row)}
function normalizeSchedules(s){return (s||[]).map(d=>({channel:Number(d.channel||d.zone||0),zone:d.zoneName||('Zone '+d.zone),startTime:d.timeValue||'00:00',durationSeconds:(d.runMinutes||1)*60,enabled:d.enabled!==false}))}
function scheduleKey(rows){return JSON.stringify(rows)}
function scheduleEditorBusy(){const a=document.activeElement;return !!(a&&a.closest&&a.closest('#adminSchedRows'))}
async function syncTime(){await fetch('/api/time/set?epoch='+Math.floor(Date.now()/1000));await refresh(true)}
async function testRemote(){const j=await (await fetch('/api/remote/test')).json();alert(JSON.stringify(j,null,2))}
async function factoryReset(){if(!confirm('Factory reset settings?'))return;const t=await (await fetch('/api/factory-reset')).text();alert(t)}
function configPayload(){return {apSsid:document.getElementById('apSsid').value,apPass:document.getElementById('apPass').value,staSsid:document.getElementById('staSsid').value,staPass:document.getElementById('staPass').value,remoteEnabled:document.getElementById('remoteEnabled').checked,remoteApiBase:document.getElementById('remoteApiBase').value,remoteDeviceId:document.getElementById('remoteDeviceId').value,remoteApiKey:document.getElementById('remoteApiKey').value,gardenLatitude:Number(document.getElementById('gardenLatitude').value||0),gardenLongitude:Number(document.getElementById('gardenLongitude').value||0),gardenTimeZone:document.getElementById('gardenTimeZone').value,gardenPosixTimeZone:document.getElementById('gardenPosixTimeZone').value}}
function hydrateConfig(s){document.getElementById('apSsid').value=s.apSsid||'';document.getElementById('apPass').value=s.apPass||'';document.getElementById('staSsid').value=s.staSsid||'';document.getElementById('staPass').value=s.staPass||'';document.getElementById('remoteEnabled').checked=!!s.remoteEnabled;document.getElementById('remoteApiBase').value=s.remoteApiBase||'';document.getElementById('remoteDeviceId').value=s.remoteDeviceId||'';document.getElementById('remoteApiKey').value=s.remoteApiKey||'';document.getElementById('gardenLatitude').value=s.gardenLatitude??'';document.getElementById('gardenLongitude').value=s.gardenLongitude??'';document.getElementById('gardenTimeZone').value=s.gardenTimeZone||'';document.getElementById('gardenPosixTimeZone').value=s.gardenPosixTimeZone||''}
async function saveConfig(){const res=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(configPayload())});if(!res.ok){alert('Failed to save device settings');return;}await refresh(true);alert('Device settings saved')}
let lastScheduleKey='';
async function saveSchedules(){const rows=[...document.querySelectorAll('#adminSchedRows .actions')];const schedules=rows.map(r=>({channel:Number(r.children[0].value),startTime:r.children[1].value,durationSeconds:Number(r.children[2].value)*60,enabled:r.children[3].value==='on'}));const res=await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({schedules})});if(!res.ok){alert('Failed to save schedules');return;}await refresh(true);alert('Schedules saved')}
async function cmd(url){await fetch(url);await refresh(true)}
async function refresh(forceScheduleRedraw){const s=await (await fetch('/api/state',{cache:'no-store'})).json();hydrateConfig(s);meta.textContent=`${s.date} ${s.time} · WiFi ${s.homeWifiConnected?s.homeIp:s.homeWifiStatus} · Master ${s.masterValveOn?'ON':'OFF'}`;const rg=document.getElementById('relay-grid');const spigotsActive=!!(s.spigotRun&&s.spigotRun.active);const zoneCards=(s.zones||[]).map(z=>{const c=colorByChannel(s.zoneColors, zoneChannel(z));const rgb=toRgb(c);return `<li class="relay-card" style="background:rgba(${rgb},0.08);border-color:rgba(${rgb},0.35)"><div><b>Zone ${z.zone}</b> <span class='${z.relayOn?'status-synced':'status-mismatch'}'>${z.relayOn?'ON':'OFF'}</span></div><div class=actions><input id="m${z.zone}" type=number min=1 max=240 value=15><button onclick="cmd('/api/manual-run?zone=${z.zone}&minutes='+encodeURIComponent(document.getElementById('m${z.zone}').value))">Run</button><button onclick="cmd('/api/relay?zone=${z.zone}&state=0')">Stop</button></div></li>`}).join('');const spigotCard=`<li class="relay-card"><div><b>Spigots</b> <span class='${spigotsActive?'status-synced':'status-mismatch'}'>${spigotsActive?'ON':'OFF'}</span></div><div class=actions><input id="m-spigots" type=number min=1 max=240 value=15><button onclick="cmd('/api/spigots-run?minutes='+encodeURIComponent(document.getElementById('m-spigots').value))">Run</button><button onclick="cmd('/api/spigots-run?action=off')">Stop</button></div></li>`;rg.innerHTML=spigotCard+zoneCards;(s.zones||[]).forEach(z=>zids(zoneChannel(z)).forEach(id=>{const e=document.getElementById(id);if(!e)return;const c=colorByChannel(s.zoneColors, zoneChannel(z));const rgb=toRgb(c);e.style.fill=`rgba(${rgb},${z.relayOn?0.55:0.2})`;e.style.stroke=`rgba(${rgb},0.9)`;e.classList.toggle('zone-active',!!z.relayOn)}));const schedules=normalizeSchedules(s.dailySchedules||[]);const nextScheduleKey=scheduleKey(schedules);const shouldRedrawSchedules=forceScheduleRedraw||nextScheduleKey!==lastScheduleKey;if(shouldRedrawSchedules){document.getElementById('timeline').innerHTML=renderTimeline(buildTimelineRows(schedules), s.zoneColors||[]);if(forceScheduleRedraw||!scheduleEditorBusy()){const rows=document.getElementById('adminSchedRows');rows.innerHTML='';(s.dailySchedules||[]).forEach(addScheduleRow);}lastScheduleKey=nextScheduleKey;}}
refresh(true);setInterval(()=>refresh(false),1000);
</script></body></html>
)rawliteral";

void handleAdmin() {
  server.send(200, "text/html", FPSTR(ADMIN_PAGE));
}

void handleSaveAdmin() {
  if (server.hasArg("apSsid")) strlcpy(apSsid, server.arg("apSsid").c_str(), sizeof(apSsid));
  if (server.hasArg("apPass")) {
    String p = server.arg("apPass");
    if (p.length() == 0 || (p.length() >= 8 && p.length() <= 63)) strlcpy(apPass, p.c_str(), sizeof(apPass));
  }
  if (server.hasArg("staSsid")) strlcpy(staSsid, server.arg("staSsid").c_str(), sizeof(staSsid));
  if (server.hasArg("staPass")) strlcpy(staPass, server.arg("staPass").c_str(), sizeof(staPass));

  remoteEnabled = server.hasArg("remoteEnabled");
  if (server.hasArg("remoteApiBase")) strlcpy(remoteApiBase, server.arg("remoteApiBase").c_str(), sizeof(remoteApiBase));
  if (server.hasArg("remoteDeviceId")) strlcpy(remoteDeviceId, server.arg("remoteDeviceId").c_str(), sizeof(remoteDeviceId));
  if (server.hasArg("remoteApiKey")) strlcpy(remoteApiKey, server.arg("remoteApiKey").c_str(), sizeof(remoteApiKey));
  if (server.hasArg("remoteIntervalSeconds")) remoteIntervalSeconds = constrain(server.arg("remoteIntervalSeconds").toInt(), 15, 3600);
  if (server.hasArg("gardenLatitude")) gardenLatitude = server.arg("gardenLatitude").toDouble();
  if (server.hasArg("gardenLongitude")) gardenLongitude = server.arg("gardenLongitude").toDouble();
  if (server.hasArg("gardenTimeZone")) {
    String tz = server.arg("gardenTimeZone");
    tz.trim();
    if (tz.length() > 0) strlcpy(gardenTimeZone, tz.c_str(), sizeof(gardenTimeZone));
  }
  if (server.hasArg("gardenPosixTimeZone")) {
    String tz = server.arg("gardenPosixTimeZone");
    tz.trim();
    if (tz.length() > 0) strlcpy(gardenPosixTimeZone, tz.c_str(), sizeof(gardenPosixTimeZone));
  }

  for (uint8_t i = 0; i < ZONE_COUNT; i++) {
    String p = "z" + String(i + 1);
    zones[i].enabled = server.hasArg(p + "enabled");
    if (server.hasArg(p + "name")) strlcpy(zones[i].name, server.arg(p + "name").c_str(), sizeof(zones[i].name));
  }

  if (server.hasArg("schedulesText")) {
    parseScheduleText(server.arg("schedulesText"));
  }

  setupGardenTimeZone();
  saveConfig();
  publishSchedulesNow();
  publishFullStateNow();
  if (strlen(staSsid) > 0) connectSta(false);

  server.sendHeader("Location", "/admin?saved=1");
  server.send(303);
}

void handleRemoteConfig() {
  StaticJsonDocument<1024> doc;
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  if (doc["enabled"].is<bool>()) remoteEnabled = doc["enabled"];
  if (doc["baseUrl"].is<const char*>()) strlcpy(remoteApiBase, doc["baseUrl"], sizeof(remoteApiBase));
  if (doc["deviceId"].is<const char*>()) strlcpy(remoteDeviceId, doc["deviceId"], sizeof(remoteDeviceId));
  if (doc["apiKey"].is<const char*>()) strlcpy(remoteApiKey, doc["apiKey"], sizeof(remoteApiKey));
  if (doc["intervalSeconds"].is<int>()) remoteIntervalSeconds = constrain((int)doc["intervalSeconds"], 15, 3600);

  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRemoteTest() {
  StaticJsonDocument<1536> out;

  String response;
  int code;

  bool stateOk = remoteGetJson("/api/state", response, code);
  out["stateReadOk"] = stateOk;
  out["stateReadCode"] = code;

  publishFullStateNow();
  out["fullStatePublishStatus"] = lastRemoteStatus;

  publishRelayStateNow();
  out["relayPublishStatus"] = lastRemoteStatus;

  publishSchedulesNow();
  out["schedulePublishStatus"] = lastRemoteStatus;

  publishSensorDataNow();
  out["sensorPublishStatus"] = lastRemoteStatus;

  out["remoteStatus"] = lastRemoteStatus;
  out["note"] = "Remote test does not call /api/queue/next so it will not consume a queued command.";

  String result;
  serializeJson(out, result);
  server.send(200, "application/json", result);
}


void setupAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIp, apGw, apMask);
  if (strlen(apPass) == 0) WiFi.softAP(apSsid);
  else WiFi.softAP(apSsid, apPass);
  dns.start(53, "*", apIp);
}

void connectSta(bool wait) {
  WiFi.mode(WIFI_AP_STA);

  if (strlen(staSsid) == 0) {
    staConnectInProgress = false;
    lastStaStatus = "home WiFi not configured";
    return;
  }

  WiFi.disconnect(false, false);
  delay(100);

  lastStaStatus = "connecting to " + String(staSsid);
  staConnectStartMs = millis();
  staConnectInProgress = true;

  if (strlen(staPass) == 0) WiFi.begin(staSsid);
  else WiFi.begin(staSsid, staPass);

  if (!wait) return;

  while (WiFi.status() != WL_CONNECTED && millis() - staConnectStartMs < 15000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    staConnectInProgress = false;
    lastStaStatus = "connected: " + WiFi.localIP().toString();
  } else {
    staConnectInProgress = false;
    lastStaStatus = "connection failed, status " + String((int)WiFi.status());
    WiFi.disconnect(false, false);
  }
}

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/admin/save", HTTP_POST, handleSaveAdmin);

  server.on("/api/state", HTTP_GET, sendStateJson);
  server.on("/api/features", HTTP_GET, handleApiFeatures);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigSet);
  server.on("/api/schedules", HTTP_POST, handleApiScheduleSet);
  server.on("/api/time/set", HTTP_GET, handleSetTime);
  server.on("/api/relay", HTTP_GET, handleRelay);
  server.on("/api/manual-run", HTTP_GET, handleManualRun);
  server.on("/api/spigots-run", HTTP_GET, handleSpigotRun);
  server.on("/api/schedule/add", HTTP_GET, handleScheduleAdd);
  server.on("/api/schedule/delete", HTTP_GET, handleScheduleDelete);
  server.on("/api/alloff", HTTP_GET, handleAllOff);
  server.on("/api/buzzer-test", HTTP_GET, handleBuzzerTest);
  server.on("/api/factory-reset", HTTP_GET, handleFactoryReset);
  server.on("/api/remote/config", HTTP_POST, handleRemoteConfig);
  server.on("/api/remote/test", HTTP_GET, handleRemoteTest);
  server.on("/status", HTTP_GET, sendStateJson);
  server.onNotFound(handleRoot);
  server.begin();
}

void remoteTask(void* param) {
  (void)param;

  for (;;) {
    if (remoteReady()) {
      serviceRemoteApi();

      // If the long poll returned a command or 204 timeout, immediately open
      // the next long poll. A short yield prevents watchdog starvation.
      vTaskDelay(pdMS_TO_TICKS(100));
    } else {
      // Weak WiFi/offline path: do not hammer the radio or Heroku.
      vTaskDelay(pdMS_TO_TICKS(REMOTE_OFFLINE_DELAY_MS));
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  loadConfig();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(RGB_LED_PIN_R, OUTPUT);
  pinMode(RGB_LED_PIN_G, OUTPUT);
  pinMode(RGB_LED_PIN_B, OUTPUT);
  setRgbLed(0, 0, 0);

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
  }
  allOff();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);

  // Set timezone from persisted/configured garden location before any schedule or clock display logic runs.
  setupGardenTimeZone();

  setupAp();
  connectSta(true);
  configTzTime(gardenPosixTimeZone, "pool.ntp.org", "time.nist.gov");

  setupServer();

  xTaskCreatePinnedToCore(
    remoteTask,
    "remoteTask",
    10000,
    nullptr,
    1,
    &remoteTaskHandle,
    0
  );

  Serial.println("GardenSimpleRelay6 ready.");
  Serial.print("AP SSID: "); Serial.println(apSsid);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();

  updateRunState();
  checkSchedule();
  updateZoneRgbLed();

  if (WiFi.status() == WL_CONNECTED && (staConnectInProgress || lastStaStatus.startsWith("connecting"))) {
    staConnectInProgress = false;
    lastStaStatus = "connected: " + WiFi.localIP().toString();
  }

  if (staConnectInProgress && WiFi.status() != WL_CONNECTED && millis() - staConnectStartMs > 20000UL) {
    staConnectInProgress = false;
    lastStaStatus = "connection timed out, status " + String((int)WiFi.status());
    WiFi.disconnect(false, false);
  }

  if (!staConnectInProgress && WiFi.status() != WL_CONNECTED && strlen(staSsid) > 0 && millis() - lastWifiAttemptMs > 60000UL) {
    lastWifiAttemptMs = millis();
    connectSta(false);
  }

  delay(10);
}
