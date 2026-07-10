// GardenSimpleRelay6.ino
// Weekday-aware composition layer for the existing relay firmware.
// The unchanged v26 implementation is included below with only its setup,
// loop, and schedule checker symbols renamed. All existing features remain
// compiled; this layer adds per-schedule weekday masks and replaces only the
// top-level scheduler call.

#define setup gardenLegacySetup
#define loop gardenLegacyLoop
#define checkSchedule gardenLegacyCheckSchedule
#include "GardenSimpleRelay6Core.inc"
#undef checkSchedule
#undef loop
#undef setup

static const uint8_t ALL_WEEKDAYS_MASK = 0x7F; // bit 0=Sunday ... bit 6=Saturday
static const char* WEEKDAY_SHORT_NAMES[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* WEEKDAY_LONG_NAMES[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

static uint8_t scheduleDaysMasks[MAX_DAILY_SCHEDULES];
static uint32_t scheduleDaysSignatures[MAX_DAILY_SCHEDULES];
static uint8_t trackedScheduleCount = 0;

uint8_t normalizeScheduleDaysMask(int value) {
  return (uint8_t)(value & ALL_WEEKDAYS_MASK);
}

String scheduleDaysMaskLabel(uint8_t mask) {
  mask = normalizeScheduleDaysMask(mask);
  if (mask == ALL_WEEKDAYS_MASK) return "All days";
  if (mask == 0x3E) return "Weekdays";
  if (mask == 0x41) return "Weekends";
  if (mask == 0) return "No days";

  String result;
  for (uint8_t day = 0; day < 7; day++) {
    if ((mask & (1U << day)) == 0) continue;
    if (result.length()) result += " ";
    result += WEEKDAY_SHORT_NAMES[day];
  }
  return result;
}

uint8_t parseScheduleDaysMask(String value) {
  value.trim();
  if (!value.length()) return ALL_WEEKDAYS_MASK;

  String lower = value;
  lower.toLowerCase();
  if (lower == "all" || lower == "all days" || lower == "daily" || lower == "everyday" || lower == "every day") return ALL_WEEKDAYS_MASK;
  if (lower == "weekdays" || lower == "weekday") return 0x3E;
  if (lower == "weekends" || lower == "weekend") return 0x41;
  if (lower == "none" || lower == "no days" || lower == "off" || lower == "disabled") return 0;

  bool numeric = true;
  for (size_t i = 0; i < lower.length(); i++) {
    if (!isDigit(lower.charAt(i))) {
      numeric = false;
      break;
    }
  }
  if (numeric) return normalizeScheduleDaysMask(lower.toInt());

  lower.replace("|", " ");
  lower.replace(",", " ");
  lower.replace("+", " ");
  lower.replace(";", " ");
  lower.replace("/", " ");

  uint8_t mask = 0;
  while (lower.length()) {
    lower.trim();
    if (!lower.length()) break;
    int split = lower.indexOf(' ');
    String token = split >= 0 ? lower.substring(0, split) : lower;
    lower = split >= 0 ? lower.substring(split + 1) : "";
    token.trim();

    if (token.startsWith("sun")) mask |= 1U << 0;
    else if (token.startsWith("mon")) mask |= 1U << 1;
    else if (token.startsWith("tue")) mask |= 1U << 2;
    else if (token.startsWith("wed")) mask |= 1U << 3;
    else if (token.startsWith("thu")) mask |= 1U << 4;
    else if (token.startsWith("fri")) mask |= 1U << 5;
    else if (token.startsWith("sat")) mask |= 1U << 6;
  }
  return normalizeScheduleDaysMask(mask);
}

uint32_t scheduleDaysSignature(const DailySchedule& schedule) {
  uint32_t hash = 2166136261UL;
  const uint32_t values[] = {
    schedule.zoneIndex,
    schedule.startHour,
    schedule.startMinute,
    schedule.runMinutes
  };
  for (uint8_t valueIndex = 0; valueIndex < 4; valueIndex++) {
    uint32_t value = values[valueIndex];
    for (uint8_t byteIndex = 0; byteIndex < 4; byteIndex++) {
      hash ^= (uint8_t)(value & 0xFFU);
      hash *= 16777619UL;
      value >>= 8;
    }
  }
  return hash;
}

void saveScheduleDaysMasks() {
  Preferences dayPrefs;
  dayPrefs.begin("relay6days", false);
  dayPrefs.putUChar("count", trackedScheduleCount);
  for (uint8_t i = 0; i < trackedScheduleCount; i++) {
    String keyBase = "s" + String(i);
    dayPrefs.putUInt((keyBase + "sig").c_str(), scheduleDaysSignatures[i]);
    dayPrefs.putUChar((keyBase + "mask").c_str(), normalizeScheduleDaysMask(scheduleDaysMasks[i]));
  }
  dayPrefs.end();
}

void reconcileScheduleDaysMasks(bool loadPersisted = false) {
  uint32_t oldSignatures[MAX_DAILY_SCHEDULES];
  uint8_t oldMasks[MAX_DAILY_SCHEDULES];
  bool oldUsed[MAX_DAILY_SCHEDULES];
  uint8_t oldCount = 0;

  if (loadPersisted) {
    Preferences dayPrefs;
    dayPrefs.begin("relay6days", true);
    oldCount = min((uint8_t)MAX_DAILY_SCHEDULES, dayPrefs.getUChar("count", 0));
    for (uint8_t i = 0; i < oldCount; i++) {
      String keyBase = "s" + String(i);
      oldSignatures[i] = dayPrefs.getUInt((keyBase + "sig").c_str(), 0);
      oldMasks[i] = normalizeScheduleDaysMask(dayPrefs.getUChar((keyBase + "mask").c_str(), ALL_WEEKDAYS_MASK));
      oldUsed[i] = false;
    }
    dayPrefs.end();
  } else {
    oldCount = trackedScheduleCount;
    for (uint8_t i = 0; i < oldCount; i++) {
      oldSignatures[i] = scheduleDaysSignatures[i];
      oldMasks[i] = scheduleDaysMasks[i];
      oldUsed[i] = false;
    }
  }

  uint8_t newCount = min((uint8_t)MAX_DAILY_SCHEDULES, dailyScheduleCount);
  bool changed = newCount != oldCount;

  for (uint8_t i = 0; i < newCount; i++) {
    uint32_t signature = scheduleDaysSignature(dailySchedules[i]);
    uint8_t mask = ALL_WEEKDAYS_MASK;
    bool matched = false;

    for (uint8_t oldIndex = 0; oldIndex < oldCount; oldIndex++) {
      if (!oldUsed[oldIndex] && oldSignatures[oldIndex] == signature) {
        oldUsed[oldIndex] = true;
        mask = oldMasks[oldIndex];
        matched = true;
        break;
      }
    }

    if (!matched) changed = true;
    if (i >= trackedScheduleCount || scheduleDaysSignatures[i] != signature || scheduleDaysMasks[i] != mask) changed = true;
    scheduleDaysSignatures[i] = signature;
    scheduleDaysMasks[i] = normalizeScheduleDaysMask(mask);
  }

  trackedScheduleCount = newCount;
  if (changed || loadPersisted) saveScheduleDaysMasks();
}

bool scheduleRunsToday(uint8_t scheduleIndex, int weekDay) {
  if (scheduleIndex >= trackedScheduleCount || weekDay < 0 || weekDay > 6) return false;
  return (scheduleDaysMasks[scheduleIndex] & (1U << weekDay)) != 0;
}

void checkScheduleWithWeekdays() {
  if (!clockIsValid()) return;

  struct tm t;
  if (!getLocalTime(&t, 20)) return;

  reconcileScheduleDaysMasks(false);

  bool startedAny = false;
  int minuteOfDay = t.tm_hour * 60 + t.tm_min;
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 || schedule.zoneIndex >= ZONE_COUNT || !zones[schedule.zoneIndex].enabled) continue;
    if (!scheduleRunsToday(i, t.tm_wday)) continue;

    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    if (minuteOfDay == scheduledMinute) {
      if (schedule.lastRunYearDay != t.tm_yday || schedule.lastRunMinuteOfDay != minuteOfDay) {
        schedule.lastRunYearDay = t.tm_yday;
        schedule.lastRunMinuteOfDay = minuteOfDay;
        startRun(schedule.zoneIndex, schedule.runMinutes, false);
        startedAny = true;
      }
    }
  }

  if (startedAny) {
    publishRelayStateNow();
    publishFullStateNow();
  }
}

String scheduleDaysPage() {
  reconcileScheduleDaysMasks(false);
  String html;
  html.reserve(14000);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Watering Days</title><style>*{box-sizing:border-box}body{font-family:Arial,sans-serif;margin:0;background:#f4f1e8;color:#18251b}main{max-width:980px;margin:auto;padding:12px}.card{background:#fff;border:1px solid #ccd5cc;border-radius:14px;padding:14px;margin:10px 0}.schedule{border-top:1px solid #d8ded8;padding:12px 0}.days{display:flex;gap:6px;flex-wrap:wrap}.day{display:inline-flex;align-items:center;gap:4px;border:1px solid #aebaae;border-radius:8px;padding:7px;background:#f8faf8}button,a.button{display:inline-block;border:0;border-radius:9px;background:#31543a;color:#fff;padding:10px 14px;text-decoration:none;font-size:16px}.muted{color:#59665c}.actions{display:flex;gap:8px;flex-wrap:wrap}</style></head><body><main>");
  html += F("<section class='card'><h1>Schedule Days of Week</h1><p class='muted'>Choose the days on which each automatic schedule may run. Manual zone and spigot runs are not affected.</p><p><a class='button' href='/admin'>Back to Admin</a></p></section>");
  html += F("<form method='post' action='/schedule-days/save'><section class='card'>");

  if (dailyScheduleCount == 0) {
    html += F("<p>No schedules configured.</p>");
  }

  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    html += F("<div class='schedule'><h3>");
    html += String(i + 1);
    html += F(". Zone ");
    html += String(schedule.zoneIndex + 1);
    html += F(" · ");
    html += compactTime(schedule.startHour, schedule.startMinute);
    html += F(" · ");
    html += String(schedule.runMinutes);
    html += F(" min</h3><p class='muted'>Current: ");
    html += scheduleDaysMaskLabel(scheduleDaysMasks[i]);
    html += F("</p><div class='days'>");
    for (uint8_t day = 0; day < 7; day++) {
      html += F("<label class='day'><input type='checkbox' name='s");
      html += String(i);
      html += F("d");
      html += String(day);
      html += F("' ");
      if (scheduleDaysMasks[i] & (1U << day)) html += F("checked ");
      html += F(">");
      html += WEEKDAY_LONG_NAMES[day];
      html += F("</label>");
    }
    html += F("</div></div>");
  }

  html += F("<div class='actions'><button type='submit'>Save Watering Days</button><button type='button' onclick=\"document.querySelectorAll('input[type=checkbox]').forEach(x=>x.checked=true)\">Select All</button><button type='button' onclick=\"document.querySelectorAll('input[type=checkbox]').forEach(x=>x.checked=false)\">Clear All</button></div></section></form>");
  html += F("</main></body></html>");
  return html;
}

void handleScheduleDaysPage() {
  server.send(200, "text/html", scheduleDaysPage());
}

void handleScheduleDaysSave() {
  reconcileScheduleDaysMasks(false);
  for (uint8_t i = 0; i < trackedScheduleCount; i++) {
    uint8_t mask = 0;
    for (uint8_t day = 0; day < 7; day++) {
      String field = "s" + String(i) + "d" + String(day);
      if (server.hasArg(field)) mask |= 1U << day;
    }
    scheduleDaysMasks[i] = normalizeScheduleDaysMask(mask);
  }
  saveScheduleDaysMasks();
  server.sendHeader("Location", "/schedule-days?saved=1");
  server.send(303, "text/plain", "");
}

void handleScheduleDaysApiGet() {
  reconcileScheduleDaysMasks(false);
  DynamicJsonDocument doc(12288);
  doc["ok"] = true;
  doc["bitOrder"] = "Sun,Mon,Tue,Wed,Thu,Fri,Sat";
  JsonArray schedules = doc.createNestedArray("schedules");
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    JsonObject item = schedules.createNestedObject();
    item["id"] = i;
    item["channel"] = dailySchedules[i].zoneIndex + 1;
    item["startTime"] = htmlTimeValue(dailySchedules[i].startHour, dailySchedules[i].startMinute);
    item["durationSeconds"] = (uint32_t)dailySchedules[i].runMinutes * 60UL;
    item["daysMask"] = scheduleDaysMasks[i];
    item["daysLabel"] = scheduleDaysMaskLabel(scheduleDaysMasks[i]);
    JsonArray days = item.createNestedArray("days");
    for (uint8_t day = 0; day < 7; day++) {
      if (scheduleDaysMasks[i] & (1U << day)) days.add(WEEKDAY_SHORT_NAMES[day]);
    }
  }
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

void handleScheduleDaysApiPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(12288);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc["schedules"].is<JsonArray>()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json or missing schedules array\"}");
    return;
  }

  reconcileScheduleDaysMasks(false);
  for (JsonObject item : doc["schedules"].as<JsonArray>()) {
    int id = item["id"] | -1;
    if (id < 0 || id >= trackedScheduleCount) continue;

    if (!item["daysMask"].isNull()) {
      scheduleDaysMasks[id] = normalizeScheduleDaysMask(item["daysMask"].as<int>());
    } else if (item["days"].is<const char*>()) {
      scheduleDaysMasks[id] = parseScheduleDaysMask(item["days"].as<String>());
    } else if (item["days"].is<JsonArray>()) {
      uint8_t mask = 0;
      for (JsonVariant day : item["days"].as<JsonArray>()) mask |= parseScheduleDaysMask(day.as<String>());
      scheduleDaysMasks[id] = normalizeScheduleDaysMask(mask);
    }
  }

  saveScheduleDaysMasks();
  server.send(200, "application/json", "{\"ok\":true}");
}

void registerScheduleDaysRoutes() {
  server.on("/schedule-days", HTTP_GET, handleScheduleDaysPage);
  server.on("/schedule-days/save", HTTP_POST, handleScheduleDaysSave);
  server.on("/api/schedule-days", HTTP_GET, handleScheduleDaysApiGet);
  server.on("/api/schedule-days", HTTP_POST, handleScheduleDaysApiPost);
}

void setup() {
  gardenLegacySetup();
  reconcileScheduleDaysMasks(true);
  registerScheduleDaysRoutes();
  Serial.println("Weekday schedule editor: /schedule-days");
  Serial.println("Weekday schedule API: /api/schedule-days");
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  if (millis() - lastWeatherFetchMs >= WEATHER_REFRESH_MS) {
    lastWeatherFetchMs = millis();
    updateWeatherFromOpenMeteo();
  }

  updateRunState();
  checkScheduleWithWeekdays();
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
