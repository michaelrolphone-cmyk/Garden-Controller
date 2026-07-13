// Master clock recovery and missed-run reconciliation.
//
// The master normally receives time through configTzTime() on its station WiFi.
// This service actively verifies recovery when the clock is invalid, can use the
// separately configured Internet-time WiFi profile, and queues local zone and
// spigot occurrences missed while the clock was unavailable.

static const uint32_t MASTER_CLOCK_INITIAL_GRACE_MS = 15000UL;
static const uint32_t MASTER_CLOCK_RETRY_MS = 60000UL;
static const uint32_t MASTER_CLOCK_CONNECT_TIMEOUT_MS = 30000UL;
static const uint16_t MASTER_CLOCK_CATCHUP_WINDOW_MINUTES = 60;

static const uint8_t MASTER_CATCHUP_ZONE = 1;
static const uint8_t MASTER_CATCHUP_SPIGOT = 2;

enum MasterClockNetworkPhase : uint8_t {
  MASTER_CLOCK_PRIMARY_NETWORK = 0,
  MASTER_CLOCK_CONNECTING_FALLBACK = 1,
  MASTER_CLOCK_RETURNING_PRIMARY = 2
};

struct MasterCatchupOccurrence {
  uint8_t kind;
  uint8_t originalIndex;
  uint8_t zoneIndex;
  uint8_t daysMask;
  uint16_t runMinutes;
  int dateKey;
  int scheduledMinute;
  uint32_t signature;
};

static TaskHandle_t masterClockRecoveryTaskHandle = nullptr;
static MasterClockNetworkPhase masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
static uint32_t masterClockServiceStartedMs = 0;
static uint32_t masterClockPhaseStartedMs = 0;
static uint32_t masterClockLastAttemptMs = 0;
static uint32_t masterClockLastEpoch = 0;
static bool masterClockPreviousValid = false;
static bool masterClockFallbackNtpAttempted = false;

static MasterCatchupOccurrence masterCatchupQueue[MAX_DAILY_SCHEDULES];
static uint8_t masterCatchupQueueCount = 0;
static uint8_t masterCatchupQueueNext = 0;
static int masterCatchupQueueDateKey = -1;
static bool masterCatchupBuildPending = false;

static uint32_t masterSpigotScheduleSignature(const SpigotSchedule& schedule);
static int masterClockDateKey(const struct tm& value);
static bool masterClockAnyLocalRunActive();
static bool masterClockExactScheduleDue(const struct tm& now, int minuteOfDay);
static int masterClockNextScheduledMinute(const struct tm& now, int minuteOfDay, int endMinute);
static void masterCatchupAppend(const MasterCatchupOccurrence& occurrence);
static bool masterBuildCatchupQueue();
static bool masterFindZoneOccurrence(const MasterCatchupOccurrence& occurrence, uint8_t& indexOut);
static bool masterFindSpigotOccurrence(const MasterCatchupOccurrence& occurrence, uint8_t& indexOut);
static void masterServiceCatchupQueue();
static bool masterClockQueryCurrentNetwork();
static void masterClockConnectFallback();
static void masterClockReturnToPrimary();
static void masterClockService();
static void masterClockRecoveryTask(void* parameter);

static uint32_t masterSpigotScheduleSignature(const SpigotSchedule& schedule) {
  uint32_t hash = 2166136261UL;
  const uint32_t values[] = {
    schedule.startHour,
    schedule.startMinute,
    schedule.runMinutes,
    schedule.daysMask
  };
  for (uint8_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    uint32_t value = values[i];
    for (uint8_t b = 0; b < 4; b++) {
      hash ^= (uint8_t)(value & 0xFFU);
      hash *= 16777619UL;
      value >>= 8;
    }
  }
  return hash;
}

static int masterClockDateKey(const struct tm& value) {
  return (value.tm_year + 1900) * 512 + value.tm_yday;
}

static bool masterClockAnyLocalRunActive() {
  return anyZoneRunActive() || spigotRun.active;
}

static bool masterClockExactScheduleDue(const struct tm& now, int minuteOfDay) {
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    uint8_t mask = i < trackedScheduleCount ? scheduleDaysMasks[i] : ALL_WEEKDAYS_MASK;
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.zoneIndex >= ZONE_COUNT || !zones[schedule.zoneIndex].enabled ||
        (mask & (1U << now.tm_wday)) == 0) {
      continue;
    }
    if (schedule.startHour * 60 + schedule.startMinute == minuteOfDay) return true;
  }

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        (schedule.daysMask & (1U << now.tm_wday)) == 0) {
      continue;
    }
    if (schedule.startHour * 60 + schedule.startMinute == minuteOfDay) return true;
  }
  return false;
}

static int masterClockNextScheduledMinute(
    const struct tm& now,
    int minuteOfDay,
    int endMinute) {
  int nextMinute = -1;
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    uint8_t mask = i < trackedScheduleCount ? scheduleDaysMasks[i] : ALL_WEEKDAYS_MASK;
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.zoneIndex >= ZONE_COUNT || !zones[schedule.zoneIndex].enabled ||
        (mask & (1U << now.tm_wday)) == 0) {
      continue;
    }
    int start = schedule.startHour * 60 + schedule.startMinute;
    if (start > minuteOfDay && start < endMinute &&
        (nextMinute < 0 || start < nextMinute)) {
      nextMinute = start;
    }
  }

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        (schedule.daysMask & (1U << now.tm_wday)) == 0) {
      continue;
    }
    int start = schedule.startHour * 60 + schedule.startMinute;
    if (start > minuteOfDay && start < endMinute &&
        (nextMinute < 0 || start < nextMinute)) {
      nextMinute = start;
    }
  }
  return nextMinute;
}

static void masterCatchupAppend(const MasterCatchupOccurrence& occurrence) {
  if (masterCatchupQueueCount >= MAX_DAILY_SCHEDULES) return;
  masterCatchupQueue[masterCatchupQueueCount++] = occurrence;
}

static bool masterBuildCatchupQueue() {
  if (!meshIsMaster() || !clockIsValid()) return false;

  struct tm now;
  if (!getLocalTime(&now, 20)) return false;

  SpigotScheduleGuard guard;
  if (!guard.acquired) {
    slaveTimeStatus = "clock recovered, but schedule storage is busy; catch-up pending";
    return false;
  }

  reconcileScheduleDaysMasks(false);
  masterCatchupQueueCount = 0;
  masterCatchupQueueNext = 0;
  masterCatchupQueueDateKey = masterClockDateKey(now);
  int minuteOfDay = now.tm_hour * 60 + now.tm_min;

  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    uint8_t mask = i < trackedScheduleCount ? scheduleDaysMasks[i] : ALL_WEEKDAYS_MASK;
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.zoneIndex >= ZONE_COUNT || !zones[schedule.zoneIndex].enabled ||
        (mask & (1U << now.tm_wday)) == 0) {
      continue;
    }

    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    int lag = minuteOfDay - scheduledMinute;
    if (lag < 0 || lag > MASTER_CLOCK_CATCHUP_WINDOW_MINUTES) continue;
    if (schedule.lastRunYearDay == now.tm_yday &&
        schedule.lastRunMinuteOfDay == scheduledMinute) {
      continue;
    }

    MasterCatchupOccurrence occurrence;
    occurrence.kind = MASTER_CATCHUP_ZONE;
    occurrence.originalIndex = i;
    occurrence.zoneIndex = schedule.zoneIndex;
    occurrence.daysMask = mask;
    occurrence.runMinutes = schedule.runMinutes;
    occurrence.dateKey = masterCatchupQueueDateKey;
    occurrence.scheduledMinute = scheduledMinute;
    occurrence.signature = scheduleDaysSignature(schedule);
    masterCatchupAppend(occurrence);
  }

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        (schedule.daysMask & (1U << now.tm_wday)) == 0) {
      continue;
    }

    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    int lag = minuteOfDay - scheduledMinute;
    if (lag < 0 || lag > MASTER_CLOCK_CATCHUP_WINDOW_MINUTES) continue;
    if (schedule.lastRunYearDay == now.tm_yday &&
        schedule.lastRunMinuteOfDay == scheduledMinute) {
      continue;
    }

    MasterCatchupOccurrence occurrence;
    occurrence.kind = MASTER_CATCHUP_SPIGOT;
    occurrence.originalIndex = i;
    occurrence.zoneIndex = 0;
    occurrence.daysMask = schedule.daysMask;
    occurrence.runMinutes = schedule.runMinutes;
    occurrence.dateKey = masterCatchupQueueDateKey;
    occurrence.scheduledMinute = scheduledMinute;
    occurrence.signature = masterSpigotScheduleSignature(schedule);
    masterCatchupAppend(occurrence);
  }

  for (uint8_t i = 1; i < masterCatchupQueueCount; i++) {
    MasterCatchupOccurrence value = masterCatchupQueue[i];
    int j = i - 1;
    while (j >= 0 &&
           masterCatchupQueue[j].scheduledMinute > value.scheduledMinute) {
      masterCatchupQueue[j + 1] = masterCatchupQueue[j];
      j--;
    }
    masterCatchupQueue[j + 1] = value;
  }

  masterCatchupBuildPending = false;
  if (masterCatchupQueueCount == 0) {
    slaveTimeStatus = "master clock recovered; no local watering occurrence was missed";
  } else {
    slaveTimeStatus = String("master clock recovered; queued ") +
        (int)masterCatchupQueueCount + " missed local watering occurrence(s)";
  }
  return true;
}

static bool masterFindZoneOccurrence(
    const MasterCatchupOccurrence& occurrence,
    uint8_t& indexOut) {
  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    uint8_t mask = i < trackedScheduleCount ? scheduleDaysMasks[i] : ALL_WEEKDAYS_MASK;
    if (scheduleDaysSignature(schedule) != occurrence.signature ||
        mask != occurrence.daysMask || !schedule.enabled ||
        schedule.zoneIndex >= ZONE_COUNT || !zones[schedule.zoneIndex].enabled) {
      continue;
    }
    if (schedule.lastRunYearDay >= 0 &&
        schedule.lastRunYearDay == occurrence.dateKey % 512 &&
        schedule.lastRunMinuteOfDay == occurrence.scheduledMinute) {
      return false;
    }
    indexOut = i;
    return true;
  }
  return false;
}

static bool masterFindSpigotOccurrence(
    const MasterCatchupOccurrence& occurrence,
    uint8_t& indexOut) {
  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];
    if (masterSpigotScheduleSignature(schedule) != occurrence.signature ||
        schedule.daysMask != occurrence.daysMask || !schedule.enabled) {
      continue;
    }
    if (schedule.lastRunYearDay >= 0 &&
        schedule.lastRunYearDay == occurrence.dateKey % 512 &&
        schedule.lastRunMinuteOfDay == occurrence.scheduledMinute) {
      return false;
    }
    indexOut = i;
    return true;
  }
  return false;
}

static void masterServiceCatchupQueue() {
  if (!meshIsMaster() || !clockIsValid() ||
      masterCatchupQueueNext >= masterCatchupQueueCount) {
    return;
  }

  struct tm now;
  if (!getLocalTime(&now, 20)) return;
  int dateKey = masterClockDateKey(now);
  if (dateKey != masterCatchupQueueDateKey) {
    masterCatchupQueueNext = masterCatchupQueueCount;
    slaveTimeStatus = "master catch-up queue expired at the civil-date boundary";
    return;
  }

  if (masterClockAnyLocalRunActive()) return;

  SpigotScheduleGuard guard;
  if (!guard.acquired) return;
  reconcileScheduleDaysMasks(false);

  int minuteOfDay = now.tm_hour * 60 + now.tm_min;
  if (masterClockExactScheduleDue(now, minuteOfDay)) return;

  while (masterCatchupQueueNext < masterCatchupQueueCount) {
    MasterCatchupOccurrence occurrence = masterCatchupQueue[masterCatchupQueueNext];
    uint8_t currentIndex = 0;
    bool stillValid = occurrence.kind == MASTER_CATCHUP_ZONE
        ? masterFindZoneOccurrence(occurrence, currentIndex)
        : masterFindSpigotOccurrence(occurrence, currentIndex);

    if (!stillValid) {
      masterCatchupQueueNext++;
      continue;
    }

    int endMinute = minuteOfDay + occurrence.runMinutes;
    if (minuteOfDay >= 20 * 60 || endMinute > 20 * 60) {
      masterCatchupQueueNext++;
      slaveTimeStatus = "missed master watering occurrence could not finish by 20:00";
      continue;
    }

    int futureStart = masterClockNextScheduledMinute(now, minuteOfDay, endMinute);
    if (futureStart >= 0) {
      slaveTimeStatus = String("master catch-up waiting for regular ") +
          two(futureStart / 60) + ":" + two(futureStart % 60) + " watering";
      return;
    }

    if (occurrence.kind == MASTER_CATCHUP_ZONE) {
      DailySchedule& schedule = dailySchedules[currentIndex];
      schedule.lastRunYearDay = now.tm_yday;
      schedule.lastRunMinuteOfDay = occurrence.scheduledMinute;
      startRun(schedule.zoneIndex, schedule.runMinutes, false);
      slaveTimeStatus = String("master clock recovery started missed Zone ") +
          (int)(schedule.zoneIndex + 1) + " watering";
    } else {
      SpigotSchedule& schedule = spigotSchedules[currentIndex];
      schedule.lastRunYearDay = now.tm_yday;
      schedule.lastRunMinuteOfDay = occurrence.scheduledMinute;
      startSpigotRun(schedule.runMinutes);
      slaveTimeStatus = "master clock recovery started missed spigot watering";
    }

    masterCatchupQueueNext++;
    publishRelayStateNow();
    publishFullStateNow();
    return;
  }

  slaveTimeStatus = "master clock recovery catch-up complete";
}

static bool masterClockQueryCurrentNetwork() {
  if (WiFi.status() != WL_CONNECTED) return false;

  uint32_t epoch = 0;
  bool synchronized = slaveTimeQueryNtp(slaveTimeNtpServer1, epoch);
  if (!synchronized && strlen(slaveTimeNtpServer2)) {
    synchronized = slaveTimeQueryNtp(slaveTimeNtpServer2, epoch);
  }
  if (!synchronized) return false;

  slaveTimeSetClock(epoch);
  slaveTimeSource = "master-internet-ntp";
  slaveTimeStatus = "master clock synchronized from verified Internet NTP";
  return true;
}

static void masterClockConnectFallback() {
  if (!slaveTimeFallbackConfigured()) return;
  WiFi.disconnect(false, false);
  delay(20);
  WiFi.begin(slaveTimeFallbackSsid, slaveTimeFallbackPass);
  staConnectInProgress = false;
  lastWifiAttemptMs = millis();
  masterClockNetworkPhase = MASTER_CLOCK_CONNECTING_FALLBACK;
  masterClockPhaseStartedMs = millis();
  masterClockFallbackNtpAttempted = false;
  slaveTimeSource = "unsynchronized";
  slaveTimeStatus = String("master clock invalid; connecting to Internet time WiFi ") +
      slaveTimeFallbackSsid;
}

static void masterClockReturnToPrimary() {
  if (!strlen(staSsid)) {
    masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
    masterClockPhaseStartedMs = millis();
    return;
  }

  WiFi.disconnect(false, false);
  delay(20);
  connectSta(false);
  masterClockNetworkPhase = MASTER_CLOCK_RETURNING_PRIMARY;
  masterClockPhaseStartedMs = millis();
}

static void masterClockService() {
  if (!meshIsMaster()) return;

  bool valid = clockIsValid();
  uint32_t currentEpoch = valid ? (uint32_t)time(nullptr) : 0;

  if (valid && !masterClockPreviousValid) {
    masterCatchupBuildPending = true;
  } else if (valid && masterClockLastEpoch > 0 &&
             currentEpoch > masterClockLastEpoch + 90UL) {
    // A forward time correction can skip scheduled minutes even though both
    // the old and new epochs pass the simple clockIsValid() threshold.
    masterCatchupBuildPending = true;
  }
  masterClockPreviousValid = valid;
  if (valid) masterClockLastEpoch = currentEpoch;

  if (masterCatchupBuildPending) masterBuildCatchupQueue();
  masterServiceCatchupQueue();

  if (valid) {
    if (masterClockNetworkPhase == MASTER_CLOCK_CONNECTING_FALLBACK &&
        slaveTimeConnectedTo(slaveTimeFallbackSsid)) {
      if (!masterClockFallbackNtpAttempted) {
        masterClockFallbackNtpAttempted = true;
        masterClockQueryCurrentNetwork();
        masterClockReturnToPrimary();
      }
      return;
    }

    if (masterClockNetworkPhase == MASTER_CLOCK_RETURNING_PRIMARY) {
      if (slaveTimeConnectedTo(staSsid) ||
          millis() - masterClockPhaseStartedMs >= MASTER_CLOCK_CONNECT_TIMEOUT_MS) {
        masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
      }
      return;
    }

    if (slaveTimeSource == "unsynchronized" || slaveTimeSource == "local") {
      slaveTimeSource = "master-system-sntp";
      if (masterCatchupQueueNext >= masterCatchupQueueCount) {
        slaveTimeStatus = "master clock valid through system NTP";
      }
    }
    return;
  }

  slaveTimeSource = "unsynchronized";
  if (masterClockNetworkPhase == MASTER_CLOCK_CONNECTING_FALLBACK) {
    if (slaveTimeConnectedTo(slaveTimeFallbackSsid)) {
      if (!masterClockFallbackNtpAttempted) {
        masterClockFallbackNtpAttempted = true;
        bool synchronized = masterClockQueryCurrentNetwork();
        if (!synchronized) {
          slaveTimeStatus = "CRITICAL: fallback WiFi connected but both NTP servers failed";
        }
        masterClockReturnToPrimary();
      }
    } else if (millis() - masterClockPhaseStartedMs >= MASTER_CLOCK_CONNECT_TIMEOUT_MS) {
      slaveTimeStatus = "CRITICAL: fallback Internet time WiFi connection failed";
      masterClockReturnToPrimary();
    }
    return;
  }

  if (masterClockNetworkPhase == MASTER_CLOCK_RETURNING_PRIMARY) {
    if (slaveTimeConnectedTo(staSsid) ||
        millis() - masterClockPhaseStartedMs >= MASTER_CLOCK_CONNECT_TIMEOUT_MS) {
      masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
    }
    return;
  }

  if (millis() - masterClockServiceStartedMs < MASTER_CLOCK_INITIAL_GRACE_MS) {
    slaveTimeStatus = "master clock invalid; waiting briefly for system NTP";
    return;
  }

  bool retryDue = masterClockLastAttemptMs == 0 ||
      millis() - masterClockLastAttemptMs >= MASTER_CLOCK_RETRY_MS;
  if (!retryDue) {
    slaveTimeStatus = slaveTimeFallbackConfigured()
        ? "CRITICAL: master clock invalid; waiting to retry verified NTP"
        : "CRITICAL: master clock invalid and fallback Internet WiFi is not configured";
    return;
  }

  masterClockLastAttemptMs = millis();
  if (WiFi.status() == WL_CONNECTED && masterClockQueryCurrentNetwork()) return;

  if (slaveTimeFallbackConfigured() &&
      !slaveTimeConnectedTo(slaveTimeFallbackSsid)) {
    masterClockConnectFallback();
    return;
  }

  slaveTimeStatus = slaveTimeFallbackConfigured()
      ? "CRITICAL: master has WiFi but verified NTP failed"
      : "CRITICAL: master clock invalid and no Internet time path is configured";
}

static void masterClockRecoveryTask(void* parameter) {
  (void)parameter;
  for (;;) {
    masterClockService();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void masterClockRecoveryPostInit() {
  if (masterClockRecoveryTaskHandle != nullptr) return;

  masterClockServiceStartedMs = millis();
  masterClockPhaseStartedMs = millis();
  masterClockLastAttemptMs = 0;
  masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
  masterClockFallbackNtpAttempted = false;
  masterCatchupQueueCount = 0;
  masterCatchupQueueNext = 0;
  masterCatchupQueueDateKey = -1;

  // If the clock was already valid before setup, do not infer that recent runs
  // were missed; this avoids duplicate catch-up after a soft reboot. If it was
  // invalid before setup but became valid during setup, queue reconciliation on
  // the first service pass.
  masterClockPreviousValid = gardenClockWasValidBeforeSetup;
  masterClockLastEpoch = masterClockPreviousValid ? (uint32_t)time(nullptr) : 0;
  masterCatchupBuildPending =
      meshIsMaster() && !gardenClockWasValidBeforeSetup && clockIsValid();

  BaseType_t created = xTaskCreatePinnedToCore(
      masterClockRecoveryTask,
      "masterClock",
      12288,
      nullptr,
      1,
      &masterClockRecoveryTaskHandle,
      0);
  if (created != pdPASS) {
    masterClockRecoveryTaskHandle = nullptr;
    slaveTimeStatus = "CRITICAL: master clock recovery task failed to start";
  }
}
