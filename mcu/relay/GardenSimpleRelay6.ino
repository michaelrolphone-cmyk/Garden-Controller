// GardenSimpleRelay6.ino
// Weekday-aware schedules for zones 1-5 plus independently timed spigot schedules.
//
// The existing 0.1.0-BETA firmware remains in GardenSimpleRelay6Core.inc.
// This layer preserves the existing relay, weather, captive portal, and remote
// behavior while adding channel-6 schedule storage, execution, and admin controls.

#define setup gardenLegacySetup
#define loop gardenLegacyLoop
#define checkSchedule gardenLegacyCheckSchedule
#define handleAdmin gardenLegacyHandleAdmin
#define setupServer gardenLegacySetupServer
#include "GardenSimpleRelay6Core.inc"
#undef setupServer
#undef handleAdmin
#undef checkSchedule
#undef loop
#undef setup

static const uint8_t ALL_WEEKDAYS_MASK = 0x7F; // bit 0=Sunday ... bit 6=Saturday
static const char* WEEKDAY_SHORT_NAMES[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

struct SpigotSchedule {
  bool enabled;
  uint8_t startHour;
  uint8_t startMinute;
  uint16_t runMinutes;
  uint8_t daysMask;
  int lastRunYearDay;
  int lastRunMinuteOfDay;
};

static uint8_t scheduleDaysMasks[MAX_DAILY_SCHEDULES];
static uint32_t scheduleDaysSignatures[MAX_DAILY_SCHEDULES];
static uint8_t trackedScheduleCount = 0;
static SpigotSchedule spigotSchedules[MAX_DAILY_SCHEDULES];
static uint8_t spigotScheduleCount = 0;
static bool spigotSchedulesDirty = false;
static TaskHandle_t spigotScheduleSyncTaskHandle = nullptr;
static const uint32_t SPIGOT_SCHEDULE_SYNC_INTERVAL_MS = 10000UL;

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

uint8_t daysMaskFromJson(JsonObject item, uint8_t fallback = ALL_WEEKDAYS_MASK) {
  if (!item["daysMask"].isNull()) {
    return normalizeScheduleDaysMask(item["daysMask"].as<int>());
  }
  if (item["days"].is<const char*>()) {
    return parseScheduleDaysMask(item["days"].as<String>());
  }
  if (item["days"].is<JsonArray>()) {
    uint8_t mask = 0;
    for (JsonVariant day : item["days"].as<JsonArray>()) {
      mask |= parseScheduleDaysMask(day.as<String>());
    }
    return normalizeScheduleDaysMask(mask);
  }
  return normalizeScheduleDaysMask(fallback);
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
      oldMasks[i] = normalizeScheduleDaysMask(
        dayPrefs.getUChar((keyBase + "mask").c_str(), ALL_WEEKDAYS_MASK)
      );
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
    if (
      i >= trackedScheduleCount ||
      scheduleDaysSignatures[i] != signature ||
      scheduleDaysMasks[i] != mask
    ) {
      changed = true;
    }

    scheduleDaysSignatures[i] = signature;
    scheduleDaysMasks[i] = normalizeScheduleDaysMask(mask);
  }

  trackedScheduleCount = newCount;
  if (changed || loadPersisted) saveScheduleDaysMasks();
}

bool zoneScheduleRunsToday(uint8_t scheduleIndex, int weekDay) {
  if (scheduleIndex >= trackedScheduleCount || weekDay < 0 || weekDay > 6) return false;
  return (scheduleDaysMasks[scheduleIndex] & (1U << weekDay)) != 0;
}

void clearSpigotSchedules() {
  spigotScheduleCount = 0;
  for (uint8_t i = 0; i < MAX_DAILY_SCHEDULES; i++) {
    spigotSchedules[i].enabled = false;
    spigotSchedules[i].startHour = 0;
    spigotSchedules[i].startMinute = 0;
    spigotSchedules[i].runMinutes = 0;
    spigotSchedules[i].daysMask = ALL_WEEKDAYS_MASK;
    spigotSchedules[i].lastRunYearDay = -1;
    spigotSchedules[i].lastRunMinuteOfDay = -1;
  }
}

bool addSpigotSchedule(
  uint8_t hour,
  uint8_t minute,
  uint16_t runMinutes,
  bool enabled,
  uint8_t daysMask
) {
  if (
    hour > 23 ||
    minute > 59 ||
    runMinutes == 0 ||
    spigotScheduleCount >= MAX_DAILY_SCHEDULES ||
    dailyScheduleCount + spigotScheduleCount >= MAX_DAILY_SCHEDULES
  ) {
    return false;
  }

  SpigotSchedule& schedule = spigotSchedules[spigotScheduleCount++];
  schedule.enabled = enabled;
  schedule.startHour = hour;
  schedule.startMinute = minute;
  schedule.runMinutes = runMinutes;
  schedule.daysMask = normalizeScheduleDaysMask(daysMask);
  schedule.lastRunYearDay = -1;
  schedule.lastRunMinuteOfDay = -1;
  return true;
}

void saveSpigotSchedules() {
  Preferences spigotPrefs;
  spigotPrefs.begin("relay6spig", false);
  spigotPrefs.putUChar("count", spigotScheduleCount);
  spigotPrefs.putBool("dirty", spigotSchedulesDirty);

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    String keyBase = "s" + String(i);
    spigotPrefs.putBool((keyBase + "en").c_str(), spigotSchedules[i].enabled);
    spigotPrefs.putUChar((keyBase + "h").c_str(), spigotSchedules[i].startHour);
    spigotPrefs.putUChar((keyBase + "m").c_str(), spigotSchedules[i].startMinute);
    spigotPrefs.putUShort((keyBase + "dur").c_str(), spigotSchedules[i].runMinutes);
    spigotPrefs.putUChar((keyBase + "mask").c_str(), spigotSchedules[i].daysMask);
  }

  spigotPrefs.end();
}

void loadSpigotSchedules() {
  clearSpigotSchedules();

  Preferences spigotPrefs;
  spigotPrefs.begin("relay6spig", true);
  uint8_t savedCount = min(
    (uint8_t)MAX_DAILY_SCHEDULES,
    spigotPrefs.getUChar("count", 0)
  );
  spigotSchedulesDirty = spigotPrefs.getBool("dirty", false);

  for (uint8_t i = 0; i < savedCount; i++) {
    String keyBase = "s" + String(i);
    bool enabled = spigotPrefs.getBool((keyBase + "en").c_str(), true);
    uint8_t hour = spigotPrefs.getUChar((keyBase + "h").c_str(), 6);
    uint8_t minute = spigotPrefs.getUChar((keyBase + "m").c_str(), 0);
    uint16_t duration = spigotPrefs.getUShort((keyBase + "dur").c_str(), 10);
    uint8_t daysMask = normalizeScheduleDaysMask(
      spigotPrefs.getUChar((keyBase + "mask").c_str(), ALL_WEEKDAYS_MASK)
    );
    addSpigotSchedule(hour, minute, duration, enabled, daysMask);
  }

  spigotPrefs.end();
}

void addDaysArray(JsonObject item, uint8_t daysMask) {
  item["daysMask"] = daysMask;
  item["daysLabel"] = scheduleDaysMaskLabel(daysMask);
  JsonArray days = item.createNestedArray("days");
  for (uint8_t day = 0; day < 7; day++) {
    if (daysMask & (1U << day)) days.add(WEEKDAY_SHORT_NAMES[day]);
  }
}

void addCombinedScheduleArray(JsonDocument& doc, const char* key = "schedules") {
  JsonArray schedules = doc.createNestedArray(key);

  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    if (schedule.zoneIndex >= ZONE_COUNT) continue;

    JsonObject item = schedules.createNestedObject();
    item["id"] = schedules.size() - 1;
    item["channel"] = schedule.zoneIndex + 1;
    item["zone"] = zones[schedule.zoneIndex].name;
    item["enabled"] = schedule.enabled;
    item["startTime"] = htmlTimeValue(schedule.startHour, schedule.startMinute);
    item["durationSeconds"] = (uint32_t)schedule.runMinutes * 60UL;
    addDaysArray(item, i < trackedScheduleCount ? scheduleDaysMasks[i] : ALL_WEEKDAYS_MASK);
  }

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];
    JsonObject item = schedules.createNestedObject();
    item["id"] = schedules.size() - 1;
    item["channel"] = MASTER_RELAY_CHANNEL;
    item["zone"] = "Spigots";
    item["enabled"] = schedule.enabled;
    item["startTime"] = htmlTimeValue(schedule.startHour, schedule.startMinute);
    item["durationSeconds"] = (uint32_t)schedule.runMinutes * 60UL;
    addDaysArray(item, schedule.daysMask);
  }
}

bool publishAllSchedulesNow() {
  if (!remoteReady()) return false;

  DynamicJsonDocument doc(16384);
  addCombinedScheduleArray(doc);

  String body;
  String response;
  int code = -1;
  serializeJson(doc, body);
  bool ok = remotePostJson("/api/microcontroller/schedules", body, response, code);
  if (ok) {
    spigotSchedulesDirty = false;
    saveSpigotSchedules();
  }
  return ok;
}

bool applyCombinedScheduleArray(JsonArray schedules, String& errorOut, bool markLocalDirty) {
  if (schedules.isNull()) {
    errorOut = "missing schedules array";
    return false;
  }
  if (schedules.size() > MAX_DAILY_SCHEDULES) {
    errorOut = "schedule limit reached";
    return false;
  }

  uint8_t zoneCount = 0;
  uint8_t spigotCount = 0;
  for (JsonObject item : schedules) {
    int channel = item["channel"] | 0;
    int durationSeconds = item["durationSeconds"] | 0;
    uint8_t hour = 0;
    uint8_t minute = 0;

    if (
      channel < 1 ||
      channel > MASTER_RELAY_CHANNEL ||
      durationSeconds <= 0 ||
      !parseStartTimeToZone(item["startTime"] | "", hour, minute)
    ) {
      errorOut = "invalid schedule row";
      return false;
    }

    if (channel == MASTER_RELAY_CHANNEL) spigotCount++;
    else zoneCount++;
  }

  if ((uint16_t)zoneCount + (uint16_t)spigotCount > MAX_DAILY_SCHEDULES) {
    errorOut = "schedule limit reached";
    return false;
  }

  DynamicJsonDocument zoneDoc(12288);
  JsonArray zoneSchedules = zoneDoc.to<JsonArray>();

  for (JsonObject item : schedules) {
    int channel = item["channel"] | 0;
    if (channel > ZONE_COUNT) continue;
    JsonObject copy = zoneSchedules.createNestedObject();
    copy.set(item);
  }

  applyScheduleArray(zoneSchedules);
  reconcileScheduleDaysMasks(false);

  clearSpigotSchedules();
  uint8_t zoneIndex = 0;

  for (JsonObject item : schedules) {
    int channel = item["channel"] | 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    parseStartTimeToZone(item["startTime"] | "", hour, minute);
    int seconds = item["durationSeconds"] | 0;
    uint16_t runMinutes = (uint16_t)constrain((seconds + 59) / 60, 1, 240);
    bool enabled = item["enabled"].is<bool>() ? (bool)item["enabled"] : true;
    uint8_t daysMask = daysMaskFromJson(item, ALL_WEEKDAYS_MASK);

    if (channel == MASTER_RELAY_CHANNEL) {
      addSpigotSchedule(hour, minute, runMinutes, enabled, daysMask);
    } else if (zoneIndex < trackedScheduleCount) {
      scheduleDaysMasks[zoneIndex++] = daysMask;
    }
  }

  saveScheduleDaysMasks();
  spigotSchedulesDirty = markLocalDirty;
  saveSpigotSchedules();

  if (markLocalDirty) {
    publishAllSchedulesNow();
  }

  return true;
}

bool syncSpigotSchedulesFromRemote() {
  if (!remoteReady()) return false;

  if (spigotSchedulesDirty && !publishAllSchedulesNow()) {
    return false;
  }

  String response;
  int code = -1;
  if (!remoteGetJson("/api/firmware/spigot-schedules", response, code)) {
    return false;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, response);
  if (error || !doc["schedules"].is<JsonArray>()) {
    lastRemoteStatus = "spigot schedule sync JSON failed";
    return false;
  }

  JsonArray schedules = doc["schedules"].as<JsonArray>();
  clearSpigotSchedules();

  for (JsonObject item : schedules) {
    int channel = item["channel"] | MASTER_RELAY_CHANNEL;
    if (channel != MASTER_RELAY_CHANNEL) continue;

    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!parseStartTimeToZone(item["startTime"] | "", hour, minute)) continue;

    int seconds = item["durationSeconds"] | 0;
    if (seconds <= 0) continue;

    bool enabled = item["enabled"].is<bool>() ? (bool)item["enabled"] : true;
    uint8_t daysMask = daysMaskFromJson(item, ALL_WEEKDAYS_MASK);
    addSpigotSchedule(
      hour,
      minute,
      (uint16_t)constrain((seconds + 59) / 60, 1, 240),
      enabled,
      daysMask
    );
  }

  spigotSchedulesDirty = false;
  saveSpigotSchedules();
  lastRemoteStatus = "spigot schedules synchronized";
  return true;
}

void spigotScheduleSyncTask(void* param) {
  (void)param;
  vTaskDelay(pdMS_TO_TICKS(3000));

  for (;;) {
    syncSpigotSchedulesFromRemote();
    vTaskDelay(pdMS_TO_TICKS(SPIGOT_SCHEDULE_SYNC_INTERVAL_MS));
  }
}

void checkScheduleWithWeekdays() {
  if (!clockIsValid()) return;

  struct tm currentTime;
  if (!getLocalTime(&currentTime, 20)) return;

  reconcileScheduleDaysMasks(false);

  bool startedAny = false;
  int minuteOfDay = currentTime.tm_hour * 60 + currentTime.tm_min;

  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];

    if (
      !schedule.enabled ||
      schedule.runMinutes == 0 ||
      schedule.zoneIndex >= ZONE_COUNT ||
      !zones[schedule.zoneIndex].enabled ||
      !zoneScheduleRunsToday(i, currentTime.tm_wday)
    ) {
      continue;
    }

    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    if (
      minuteOfDay == scheduledMinute &&
      (
        schedule.lastRunYearDay != currentTime.tm_yday ||
        schedule.lastRunMinuteOfDay != minuteOfDay
      )
    ) {
      schedule.lastRunYearDay = currentTime.tm_yday;
      schedule.lastRunMinuteOfDay = minuteOfDay;
      startRun(schedule.zoneIndex, schedule.runMinutes, false);
      startedAny = true;
    }
  }

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];

    if (
      !schedule.enabled ||
      schedule.runMinutes == 0 ||
      (schedule.daysMask & (1U << currentTime.tm_wday)) == 0
    ) {
      continue;
    }

    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    if (
      minuteOfDay == scheduledMinute &&
      (
        schedule.lastRunYearDay != currentTime.tm_yday ||
        schedule.lastRunMinuteOfDay != minuteOfDay
      )
    ) {
      schedule.lastRunYearDay = currentTime.tm_yday;
      schedule.lastRunMinuteOfDay = minuteOfDay;
      startSpigotRun(schedule.runMinutes);
      startedAny = true;
    }
  }

  if (startedAny) {
    publishRelayStateNow();
    publishFullStateNow();
  }
}

void handleScheduleDaysRedirect() {
  server.sendHeader("Location", "/admin#schedule-manager", true);
  server.send(302, "text/plain", "");
}

void handleScheduleDaysApiGet() {
  reconcileScheduleDaysMasks(false);

  DynamicJsonDocument doc(16384);
  doc["ok"] = true;
  doc["bitOrder"] = "Sun,Mon,Tue,Wed,Thu,Fri,Sat";
  addCombinedScheduleArray(doc);

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
    if (id < 0) continue;

    if (id < trackedScheduleCount) {
      scheduleDaysMasks[id] = daysMaskFromJson(item, scheduleDaysMasks[id]);
      continue;
    }

    int spigotIndex = id - trackedScheduleCount;
    if (spigotIndex >= 0 && spigotIndex < spigotScheduleCount) {
      spigotSchedules[spigotIndex].daysMask = daysMaskFromJson(
        item,
        spigotSchedules[spigotIndex].daysMask
      );
    }
  }

  saveScheduleDaysMasks();
  spigotSchedulesDirty = true;
  saveSpigotSchedules();
  publishAllSchedulesNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSchedulesWithDaysApiPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc["schedules"].is<JsonArray>()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json or missing schedules array\"}");
    return;
  }

  String applyError;
  if (!applyCombinedScheduleArray(doc["schedules"].as<JsonArray>(), applyError, true)) {
    DynamicJsonDocument errorDoc(256);
    errorDoc["ok"] = false;
    errorDoc["error"] = applyError;
    String body;
    serializeJson(errorDoc, body);
    server.send(400, "application/json", body);
    return;
  }

  publishFullStateNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

const char SCHEDULE_ADMIN_CSS[] PROGMEM = R"rawliteral(
.schedule-editor-row{border:1px solid #d8e3eb;border-radius:12px;padding:10px;margin:10px 0;background:#f8fbfd}
.schedule-editor-fields{display:grid;grid-template-columns:140px 120px 110px 120px auto;gap:8px;align-items:end}
.schedule-editor-fields label{min-width:0}
.schedule-editor-fields input,.schedule-editor-fields select{width:100%;min-width:0}
.weekday-picker{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
.weekday-choice{display:inline-flex;flex-direction:row;align-items:center;gap:4px;border:1px solid #b9cbd7;border-radius:999px;padding:6px 9px;background:#fff;font-size:.85rem}
.weekday-choice input{width:auto;margin:0}
.weekday-presets{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}
.weekday-presets button{padding:5px 8px;font-size:.8rem;background:#526f5a}
.schedule-days-summary{font-size:.82rem;color:#4a5e6b;margin-top:7px}
@media(max-width:760px){
  .schedule-editor-fields{grid-template-columns:1fr 1fr}
  .schedule-editor-fields .delete-schedule{grid-column:1/-1}
}
)rawliteral";

const char SCHEDULE_ADMIN_SCRIPT[] PROGMEM = R"rawliteral(
const WEEKDAY_NAMES=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
let combinedScheduleRows=[];
let lastCombinedScheduleKey='';

function weekdayMaskForRow(row){
  return [...row.querySelectorAll('.schedule-day')].reduce(
    (mask,input)=>input.checked?(mask|Number(input.value)):mask,
    0
  );
}

function weekdayLabel(mask){
  if(mask===127)return 'All days';
  if(mask===62)return 'Weekdays';
  if(mask===65)return 'Weekends';
  if(mask===0)return 'No automatic watering days';
  return WEEKDAY_NAMES.filter((_,i)=>mask&(1<<i)).join(' ');
}

function updateWeekdaySummary(row){
  const target=row.querySelector('.schedule-days-summary');
  if(target)target.textContent=weekdayLabel(weekdayMaskForRow(row));
}

function setRowWeekdayMask(button,mask){
  const row=button.closest('.schedule-editor-row');
  row.querySelectorAll('.schedule-day').forEach(input=>{
    input.checked=Boolean(mask&Number(input.value));
  });
  updateWeekdaySummary(row);
}

function channelOptions(selected){
  return [
    [1,'Zone 1'],
    [2,'Zone 2'],
    [3,'Zone 3'],
    [4,'Zone 4'],
    [5,'Zone 5'],
    [6,'Spigots']
  ].map(([value,label])=>`<option value="${value}"${Number(selected)===value?' selected':''}>${label}</option>`).join('');
}

addScheduleRow=function(v){
  const rows=document.getElementById('adminSchedRows');
  const row=document.createElement('div');
  row.className='schedule-editor-row';

  const channel=Number(v&&(v.channel??v.zone)||1);
  const time=(v&&(v.startTime||v.timeValue))||'06:00';
  const minutes=Number(v&&(v.runMinutes||Math.round(Number(v.durationSeconds||600)/60))||10);
  const enabled=!(v&&v.enabled===false);
  const mask=Number(v&&v.daysMask!=null?v.daysMask:127);

  row.innerHTML=`
    <div class="schedule-editor-fields">
      <label>Valve
        <select class="schedule-zone">${channelOptions(channel)}</select>
      </label>
      <label>Start
        <input class="schedule-time" type="time" value="${time}">
      </label>
      <label>Minutes
        <input class="schedule-minutes" type="number" min="1" max="240" value="${minutes}">
      </label>
      <label>Enabled
        <select class="schedule-enabled">
          <option value="on"${enabled?' selected':''}>On</option>
          <option value="off"${enabled?'':' selected'}>Off</option>
        </select>
      </label>
      <button class="danger delete-schedule" type="button" onclick="this.closest('.schedule-editor-row').remove()">Delete</button>
    </div>
    <div class="weekday-picker">
      ${WEEKDAY_NAMES.map((name,index)=>`
        <label class="weekday-choice">
          <input class="schedule-day" type="checkbox" value="${1<<index}"${mask&(1<<index)?' checked':''}>
          ${name}
        </label>
      `).join('')}
    </div>
    <div class="weekday-presets">
      <button type="button" onclick="setRowWeekdayMask(this,127)">All days</button>
      <button type="button" onclick="setRowWeekdayMask(this,62)">Weekdays</button>
      <button type="button" onclick="setRowWeekdayMask(this,65)">Weekends</button>
      <button type="button" onclick="setRowWeekdayMask(this,0)">No days</button>
    </div>
    <div class="schedule-days-summary"></div>
  `;

  row.querySelectorAll('.schedule-day').forEach(input=>{
    input.addEventListener('change',()=>updateWeekdaySummary(row));
  });

  rows.appendChild(row);
  updateWeekdaySummary(row);
};

saveSchedules=async function(){
  const rows=[...document.querySelectorAll('#adminSchedRows .schedule-editor-row')];

  const schedules=rows.map((row,index)=>{
    const channel=Number(row.querySelector('.schedule-zone').value);
    return {
      id:index,
      channel,
      zone:channel===6?'Spigots':('Zone '+channel),
      startTime:row.querySelector('.schedule-time').value,
      durationSeconds:Number(row.querySelector('.schedule-minutes').value)*60,
      enabled:row.querySelector('.schedule-enabled').value==='on',
      daysMask:weekdayMaskForRow(row)
    };
  });

  const response=await fetch('/api/schedules-with-days',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({schedules})
  });

  if(!response.ok){
    let message='Failed to save schedules';
    try{
      const body=await response.json();
      if(body&&body.error)message+=': '+body.error;
    }catch(error){}
    alert(message);
    return;
  }

  await refresh(true);
  alert('Zone and spigot schedules saved');
};

function combinedRowsForAdmin(){
  return combinedScheduleRows.map(item=>({
    id:Number(item.id),
    channel:Number(item.channel),
    zone:Number(item.channel),
    zoneName:item.zone||(
      Number(item.channel)===6?'Spigots':('Zone '+item.channel)
    ),
    startTime:item.startTime,
    timeValue:item.startTime,
    durationSeconds:Number(item.durationSeconds)||60,
    runMinutes:Math.max(1,Math.round((Number(item.durationSeconds)||60)/60)),
    enabled:item.enabled!==false,
    daysMask:Number(item.daysMask??127)
  }));
}

const refreshWithoutScheduleChannels=refresh;
refresh=async function(forceScheduleRedraw){
  let combinedChanged=false;

  try{
    const response=await fetch('/api/schedule-days',{cache:'no-store'});
    if(response.ok){
      const data=await response.json();
      combinedScheduleRows=data.schedules||[];
      const nextKey=JSON.stringify(combinedScheduleRows.map(item=>[
        Number(item.id),
        Number(item.channel),
        String(item.startTime),
        Number(item.durationSeconds),
        item.enabled!==false,
        Number(item.daysMask)
      ]));
      combinedChanged=nextKey!==lastCombinedScheduleKey;
      lastCombinedScheduleKey=nextKey;
    }
  }catch(error){
    console.warn('Unable to load combined schedules',error);
  }

  await refreshWithoutScheduleChannels(Boolean(forceScheduleRedraw));

  if(forceScheduleRedraw||combinedChanged){
    const normalized=normalizeSchedules(combinedRowsForAdmin());
    document.getElementById('timeline').innerHTML=renderTimeline(
      buildTimelineRows(normalized),
      []
    );

    if(forceScheduleRedraw||!scheduleEditorBusy()){
      const rows=document.getElementById('adminSchedRows');
      rows.innerHTML='';
      combinedRowsForAdmin().forEach(addScheduleRow);
    }
  }
};

refresh(true);
setInterval(()=>refresh(false),1000);
)rawliteral";

bool replaceAdminFragment(
  String& page,
  const String& original,
  const String& replacement,
  const char* label
) {
  if (page.indexOf(original) < 0) {
    Serial.print("Admin schedule integration fragment missing: ");
    Serial.println(label);
    return false;
  }

  page.replace(original, replacement);
  return true;
}

String buildIntegratedAdminPage() {
  String page = String(FPSTR(ADMIN_PAGE));
  page.reserve(page.length() + 16000);

  replaceAdminFragment(
    page,
    "</style></head>",
    String(FPSTR(SCHEDULE_ADMIN_CSS)) + "</style></head>",
    "styles"
  );

  replaceAdminFragment(
    page,
    "<section class=\"panel\"><h2>Zones (scheduled irrigation)</h2>",
    "<section class=\"panel\" id=\"schedule-manager\"><h2>Zones and Spigots</h2>",
    "schedule manager anchor"
  );

  replaceAdminFragment(
    page,
    "<h3>Schedule Manager</h3><p>Create, update, and delete schedule rows directly in firmware.</p>",
    "<h3>Schedule Manager</h3><p>Schedule Zones 1-5 or the Spigots. Each row stores start time, duration, enabled state, and Sunday-through-Saturday watering days. Channel 6 runs only the spigots; it does not energize any zone valve.</p>",
    "schedule manager instructions"
  );

  replaceAdminFragment(
    page,
    "refresh(true);setInterval(()=>refresh(false),1000);",
    String(FPSTR(SCHEDULE_ADMIN_SCRIPT)),
    "schedule manager script"
  );

  return page;
}

void handleAdmin() {
  server.send(200, "text/html", buildIntegratedAdminPage());
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
  server.on("/api/schedules-with-days", HTTP_POST, handleSchedulesWithDaysApiPost);
  server.on("/api/time/set", HTTP_GET, handleSetTime);
  server.on("/time", HTTP_GET, handleTimeGet);
  server.on("/weather", HTTP_GET, handleWeatherGet);
  server.on("/api/relay", HTTP_GET, handleRelay);
  server.on("/api/manual-run", HTTP_GET, handleManualRun);
  server.on("/api/zone/stop", HTTP_GET, handleStopZone);
  server.on("/api/spigots-run", HTTP_GET, handleSpigotRun);
  server.on("/api/schedule/add", HTTP_GET, handleScheduleAdd);
  server.on("/api/schedule/delete", HTTP_GET, handleScheduleDelete);
  server.on("/api/alloff", HTTP_GET, handleAllOff);
  server.on("/api/buzzer-test", HTTP_GET, handleBuzzerTest);
  server.on("/api/factory-reset", HTTP_GET, handleFactoryReset);
  server.on("/api/remote/config", HTTP_POST, handleRemoteConfig);
  server.on("/api/remote/test", HTTP_GET, handleRemoteTest);

  server.on("/schedule-days", HTTP_GET, handleScheduleDaysRedirect);
  server.on("/schedule-days/save", HTTP_POST, handleScheduleDaysRedirect);
  server.on("/api/schedule-days", HTTP_GET, handleScheduleDaysApiGet);
  server.on("/api/schedule-days", HTTP_POST, handleScheduleDaysApiPost);

  server.on("/status", HTTP_GET, sendStateJson);
  server.onNotFound(handleRoot);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  loadConfig();
  reconcileScheduleDaysMasks(true);
  loadSpigotSchedules();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  statusPixel.begin();
  statusPixel.setBrightness(RGB_LED_BRIGHTNESS);
  statusPixel.clear();
  statusPixel.show();

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
  }
  allOff();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);

  setupGardenTimeZone();
  setupAp();
  connectSta(true);
  configTzTime(gardenPosixTimeZone, "pool.ntp.org", "time.nist.gov");
  updateWeatherFromOpenMeteo();

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

  xTaskCreatePinnedToCore(
    spigotScheduleSyncTask,
    "spigotScheduleSync",
    8192,
    nullptr,
    1,
    &spigotScheduleSyncTaskHandle,
    0
  );

  Serial.println("GardenSimpleRelay6 ready.");
  Serial.println("Zone and spigot schedules are managed in captive portal /admin.");
  Serial.print("AP SSID: ");
  Serial.println(apSsid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
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

  if (
    WiFi.status() == WL_CONNECTED &&
    (staConnectInProgress || lastStaStatus.startsWith("connecting"))
  ) {
    staConnectInProgress = false;
    lastStaStatus = "connected: " + WiFi.localIP().toString();
  }

  if (
    staConnectInProgress &&
    WiFi.status() != WL_CONNECTED &&
    millis() - staConnectStartMs > 20000UL
  ) {
    staConnectInProgress = false;
    lastStaStatus = "connection timed out, status " + String((int)WiFi.status());
    WiFi.disconnect(false, false);
  }

  if (
    !staConnectInProgress &&
    WiFi.status() != WL_CONNECTED &&
    strlen(staSsid) > 0 &&
    millis() - lastWifiAttemptMs > 60000UL
  ) {
    lastWifiAttemptMs = millis();
    connectSta(false);
  }

  delay(10);
}
