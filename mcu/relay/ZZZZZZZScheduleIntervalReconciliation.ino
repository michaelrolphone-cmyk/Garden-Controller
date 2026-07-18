// Schedule interval reconciliation.
//
// A schedule is an absolute civil-time interval. While the current time lies
// inside that interval the relay must be on; outside the interval a relay that
// was started by the schedule must be off. Re-evaluating an active interval is
// idempotent and never restarts the full configured duration.

static const uint32_t SCHEDULE_INTERVAL_SERVICE_MS = 250UL;
static TaskHandle_t scheduleIntervalTaskHandle = nullptr;
static bool masterZoneIntervalOwned[ZONE_COUNT] = {false, false, false, false, false};
static bool masterSpigotIntervalOwned = false;

struct ActiveScheduleInterval {
  bool active;
  uint32_t remainingSeconds;
  int16_t scheduleIndex;
};

static uint32_t intervalCurrentSecond(const struct tm& now) {
  return (uint32_t)now.tm_hour * 3600UL +
      (uint32_t)now.tm_min * 60UL +
      (uint32_t)now.tm_sec;
}

static uint32_t intervalRemainingForSchedule(
    uint8_t startHour,
    uint8_t startMinute,
    uint16_t runMinutes,
    uint8_t daysMask,
    const struct tm& now) {
  if (runMinutes == 0) return 0;

  uint32_t current = intervalCurrentSecond(now);
  uint32_t start = (uint32_t)startHour * 3600UL +
      (uint32_t)startMinute * 60UL;
  uint32_t duration = (uint32_t)runMinutes * 60UL;
  uint32_t end = start + duration;

  if ((daysMask & (1U << now.tm_wday)) != 0 &&
      current >= start && current < end) {
    return end - current;
  }

  // Preserve correct behavior for a local schedule that crosses midnight.
  uint8_t previousWeekday = (uint8_t)((now.tm_wday + 6) % 7);
  if (end > 24UL * 3600UL &&
      (daysMask & (1U << previousWeekday)) != 0) {
    uint32_t carryEnd = end - 24UL * 3600UL;
    if (current < carryEnd) return carryEnd - current;
  }

  return 0;
}

static uint32_t intervalRunRemainingMs(
    uint32_t startedMs,
    uint32_t durationMs) {
  uint32_t elapsed = millis() - startedMs;
  return durationMs > elapsed ? durationMs - elapsed : 0;
}

static void intervalSetMasterZoneDeadline(
    uint8_t zoneIndex,
    uint32_t remainingSeconds) {
  if (zoneIndex >= ZONE_COUNT || remainingSeconds == 0) return;

  uint32_t remainingMs = remainingSeconds * 1000UL;
  if (!zoneRuns[zoneIndex].active) {
    startRunSeconds(zoneIndex, remainingSeconds, false);
    masterZoneIntervalOwned[zoneIndex] = true;
    return;
  }

  if (!zoneRuns[zoneIndex].manual || masterZoneIntervalOwned[zoneIndex]) {
    zoneRuns[zoneIndex].startedMs = millis();
    zoneRuns[zoneIndex].durationMs = remainingMs;
    zoneRuns[zoneIndex].manual = false;
    masterZoneIntervalOwned[zoneIndex] = true;
    if (!relayState[zoneIndex]) setZoneRelay(zoneIndex, true);
    return;
  }

  // A manual run remains a manual run, but it may not expire before an active
  // scheduled interval ends.
  uint32_t currentRemaining = intervalRunRemainingMs(
      zoneRuns[zoneIndex].startedMs,
      zoneRuns[zoneIndex].durationMs);
  if (currentRemaining < remainingMs) {
    uint32_t elapsed = millis() - zoneRuns[zoneIndex].startedMs;
    zoneRuns[zoneIndex].durationMs = elapsed + remainingMs;
  }
  if (!relayState[zoneIndex]) setZoneRelay(zoneIndex, true);
}

static void intervalStopMasterZoneIfOwned(uint8_t zoneIndex) {
  if (zoneIndex >= ZONE_COUNT) return;
  if (masterZoneIntervalOwned[zoneIndex] &&
      zoneRuns[zoneIndex].active &&
      !zoneRuns[zoneIndex].manual) {
    stopZone(zoneIndex);
  }
  if (!zoneRuns[zoneIndex].active || zoneRuns[zoneIndex].manual) {
    masterZoneIntervalOwned[zoneIndex] = false;
  }
}

static void intervalSetMasterSpigotDeadline(
    uint32_t remainingSeconds,
    bool legacyScheduleStarted) {
  if (remainingSeconds == 0) return;
  uint32_t remainingMs = remainingSeconds * 1000UL;

  if (!spigotRun.active) {
    startSpigotRunSeconds(remainingSeconds);
    masterSpigotIntervalOwned = true;
    return;
  }

  if (masterSpigotIntervalOwned || legacyScheduleStarted) {
    spigotRun.startedMs = millis();
    spigotRun.durationMs = remainingMs;
    masterSpigotIntervalOwned = true;
    updateMasterValve();
    return;
  }

  // Preserve an independently requested manual spigot run, but guarantee that
  // it cannot expire before the active schedule interval ends.
  uint32_t currentRemaining = intervalRunRemainingMs(
      spigotRun.startedMs,
      spigotRun.durationMs);
  if (currentRemaining < remainingMs) {
    uint32_t elapsed = millis() - spigotRun.startedMs;
    spigotRun.durationMs = elapsed + remainingMs;
  }
  updateMasterValve();
}

static void intervalReconcileMaster(const struct tm& now) {
  ActiveScheduleInterval zonesActive[ZONE_COUNT];
  for (uint8_t zone = 0; zone < ZONE_COUNT; zone++) {
    zonesActive[zone] = {false, 0, -1};
  }
  ActiveScheduleInterval spigotActive = {false, 0, -1};
  bool spigotLegacyStarted = false;

  SpigotScheduleGuard guard;
  if (!guard.acquired) return;
  reconcileScheduleDaysMasks(false);

  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    DailySchedule& schedule = dailySchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.zoneIndex >= ZONE_COUNT || !zones[schedule.zoneIndex].enabled) {
      continue;
    }
    uint8_t mask = i < trackedScheduleCount
        ? scheduleDaysMasks[i]
        : ALL_WEEKDAYS_MASK;
    uint32_t remaining = intervalRemainingForSchedule(
        schedule.startHour,
        schedule.startMinute,
        schedule.runMinutes,
        mask,
        now);
    if (remaining == 0) continue;

    ActiveScheduleInterval& desired = zonesActive[schedule.zoneIndex];
    if (!desired.active || remaining > desired.remainingSeconds) {
      desired.active = true;
      desired.remainingSeconds = remaining;
      desired.scheduleIndex = i;
    }

    // Suppress the legacy exact-minute start path. The interval reconciler is
    // authoritative and supplies only the time remaining until the fixed end.
    schedule.lastRunYearDay = now.tm_yday;
    schedule.lastRunMinuteOfDay =
        schedule.startHour * 60 + schedule.startMinute;
  }

  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    SpigotSchedule& schedule = spigotSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0) continue;
    uint32_t remaining = intervalRemainingForSchedule(
        schedule.startHour,
        schedule.startMinute,
        schedule.runMinutes,
        schedule.daysMask,
        now);
    if (remaining == 0) continue;

    if (!spigotActive.active || remaining > spigotActive.remainingSeconds) {
      spigotActive.active = true;
      spigotActive.remainingSeconds = remaining;
      spigotActive.scheduleIndex = i;
    }
    if (schedule.lastRunYearDay == now.tm_yday &&
        schedule.lastRunMinuteOfDay ==
            schedule.startHour * 60 + schedule.startMinute) {
      spigotLegacyStarted = true;
    }
    schedule.lastRunYearDay = now.tm_yday;
    schedule.lastRunMinuteOfDay =
        schedule.startHour * 60 + schedule.startMinute;
  }

  for (uint8_t zone = 0; zone < ZONE_COUNT; zone++) {
    if (zonesActive[zone].active) {
      intervalSetMasterZoneDeadline(zone, zonesActive[zone].remainingSeconds);
    } else {
      intervalStopMasterZoneIfOwned(zone);
    }
  }

  if (spigotActive.active) {
    intervalSetMasterSpigotDeadline(
        spigotActive.remainingSeconds,
        spigotLegacyStarted);
  } else if (masterSpigotIntervalOwned) {
    stopSpigotRun();
    masterSpigotIntervalOwned = false;
  } else if (!spigotRun.active) {
    masterSpigotIntervalOwned = false;
  }
}

static void intervalSetSlaveScheduleDeadline(
    uint8_t relayIndex,
    uint32_t remainingSeconds,
    int16_t scheduleIndex) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE || remainingSeconds == 0) return;
  uint32_t remainingMs = remainingSeconds * 1000UL;
  bool scheduleOwned =
      reliableActiveRunSequence[relayIndex] != 0 &&
      strcmp(reliableActiveSource[relayIndex], "schedule") == 0;

  if (!meshSlaveRuns[relayIndex].active || !relayState[relayIndex]) {
    if (reliableActiveRunSequence[relayIndex] != 0) {
      reliableStopSlaveRelay(relayIndex, "reconciled");
    }
    reliableStartSlaveRelay(
        relayIndex,
        remainingSeconds,
        "schedule",
        scheduleIndex);
    return;
  }

  if (scheduleOwned) {
    meshSlaveRuns[relayIndex].startedMs = millis();
    meshSlaveRuns[relayIndex].durationMs = remainingMs;
    reliableActiveDurationSeconds[relayIndex] = remainingSeconds;
    reliableActiveScheduleIndex[relayIndex] = scheduleIndex;
    if (!relayState[relayIndex]) setRelay(relayIndex, true);
    return;
  }

  // A manual or master-requested run is not converted into a schedule run, but
  // it must remain on for at least the active schedule interval.
  uint32_t currentRemaining = intervalRunRemainingMs(
      meshSlaveRuns[relayIndex].startedMs,
      meshSlaveRuns[relayIndex].durationMs);
  if (currentRemaining < remainingMs) {
    uint32_t elapsed = millis() - meshSlaveRuns[relayIndex].startedMs;
    meshSlaveRuns[relayIndex].durationMs = elapsed + remainingMs;
    reliableActiveDurationSeconds[relayIndex] =
        max(reliableActiveDurationSeconds[relayIndex], remainingSeconds);
  }
  if (!relayState[relayIndex]) setRelay(relayIndex, true);
}

static void intervalReconcileSlave(const struct tm& now) {
  ActiveScheduleInterval active[MESH_RELAYS_PER_SLAVE];
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    active[relay] = {false, 0, -1};
  }

  MeshGuard guard;
  if (!guard.acquired) return;
  bool scheduleMarkersChanged = false;

  for (uint8_t i = 0; i < reliableSlaveScheduleCount; i++) {
    ReliableSchedule& schedule = reliableSlaveSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.localRelay < 1 ||
        schedule.localRelay > MESH_RELAYS_PER_SLAVE) {
      continue;
    }

    uint32_t remaining = intervalRemainingForSchedule(
        schedule.startHour,
        schedule.startMinute,
        schedule.runMinutes,
        schedule.daysMask,
        now);
    if (remaining == 0) continue;

    uint8_t relayIndex = schedule.localRelay - 1;
    if (!active[relayIndex].active ||
        remaining > active[relayIndex].remainingSeconds) {
      active[relayIndex].active = true;
      active[relayIndex].remainingSeconds = remaining;
      active[relayIndex].scheduleIndex = i;
    }

    int dateKey = (now.tm_year + 1900) * 512 + now.tm_yday;
    int startMinute = schedule.startHour * 60 + schedule.startMinute;
    if (schedule.lastRunDateKey != dateKey ||
        schedule.lastRunMinuteOfDay != startMinute) {
      schedule.lastRunDateKey = dateKey;
      schedule.lastRunMinuteOfDay = startMinute;
      scheduleMarkersChanged = true;
    }
  }

  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    if (active[relay].active) {
      intervalSetSlaveScheduleDeadline(
          relay,
          active[relay].remainingSeconds,
          active[relay].scheduleIndex);
      continue;
    }

    bool scheduleOwned =
        reliableActiveRunSequence[relay] != 0 &&
        strcmp(reliableActiveSource[relay], "schedule") == 0;
    if (scheduleOwned) {
      reliableStopSlaveRelay(relay, "completed");
    }
  }

  if (scheduleMarkersChanged) reliableSaveSlaveSchedules();
}

static void scheduleIntervalTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (clockIsValid()) {
      struct tm now;
      if (getLocalTime(&now, 20)) {
        if (meshIsSlave()) {
          if (meshLegacyNeutralized) intervalReconcileSlave(now);
        } else {
          intervalReconcileMaster(now);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(SCHEDULE_INTERVAL_SERVICE_MS));
  }
}

void scheduleIntervalReconciliationPostInit() {
  if (scheduleIntervalTaskHandle != nullptr) return;
  BaseType_t created = xTaskCreatePinnedToCore(
      scheduleIntervalTask,
      "scheduleIntervals",
      12288,
      nullptr,
      2,
      &scheduleIntervalTaskHandle,
      0);
  if (created != pdPASS) {
    scheduleIntervalTaskHandle = nullptr;
    meshLastStatus = "CRITICAL: schedule interval reconciliation failed to start";
    slaveTimeStatus = "CRITICAL: schedule interval service unavailable";
  }
}
