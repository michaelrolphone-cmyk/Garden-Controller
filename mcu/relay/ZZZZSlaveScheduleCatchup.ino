// Catch up a recently missed slave schedule after the clock becomes valid.
//
// The normal reliable scheduler runs exact-minute matches. Following a cold
// boot with neither master nor valid RTC time, NTP may complete shortly after a
// scheduled minute. This bounded catch-up prevents that clock-acquisition delay
// from silently dropping the watering run.

static const uint16_t SLAVE_CLOCK_CATCHUP_WINDOW_MINUTES = 60;
static bool slaveCatchupPreviousClockValid = false;
static int slaveCatchupEvaluatedDateKey = -1;
static TaskHandle_t slaveCatchupTaskHandle = nullptr;

static bool slaveCatchupAnyRelayActive() {
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    if (meshSlaveRuns[relay].active || relayState[relay]) return true;
  }
  return false;
}

static bool slaveCatchupCurrentMinuteHasSchedule(
    const struct tm& now,
    int minuteOfDay) {
  for (uint8_t i = 0; i < reliableSlaveScheduleCount; i++) {
    ReliableSchedule& schedule = reliableSlaveSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.localRelay < 1 ||
        schedule.localRelay > MESH_RELAYS_PER_SLAVE ||
        (schedule.daysMask & (1U << now.tm_wday)) == 0) {
      continue;
    }
    if (schedule.startHour * 60 + schedule.startMinute == minuteOfDay) {
      return true;
    }
  }
  return false;
}

static void slaveCatchupAfterClockRecovery() {
  if (!meshIsSlave() || !clockIsValid() || !meshLegacyNeutralized) return;

  struct tm now;
  if (!getLocalTime(&now, 20)) return;
  int minuteOfDay = now.tm_hour * 60 + now.tm_min;
  int dateKey = (now.tm_year + 1900) * 512 + now.tm_yday;
  if (slaveCatchupEvaluatedDateKey == dateKey) return;

  // Let an exact-minute schedule win and avoid creating an overlap with an
  // already active manual or scheduled relay run.
  if (slaveCatchupAnyRelayActive() ||
      slaveCatchupCurrentMinuteHasSchedule(now, minuteOfDay)) {
    slaveCatchupEvaluatedDateKey = dateKey;
    return;
  }

  int selected = -1;
  int selectedLag = INT_MAX;
  for (uint8_t i = 0; i < reliableSlaveScheduleCount; i++) {
    ReliableSchedule& schedule = reliableSlaveSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.localRelay < 1 ||
        schedule.localRelay > MESH_RELAYS_PER_SLAVE ||
        (schedule.daysMask & (1U << now.tm_wday)) == 0) {
      continue;
    }

    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    int lag = minuteOfDay - scheduledMinute;
    if (lag <= 0 || lag > SLAVE_CLOCK_CATCHUP_WINDOW_MINUTES) continue;
    if (schedule.lastRunDateKey == dateKey) continue;
    if (minuteOfDay + schedule.runMinutes > 20 * 60) continue;
    if (lag < selectedLag) {
      selected = i;
      selectedLag = lag;
    }
  }

  slaveCatchupEvaluatedDateKey = dateKey;
  if (selected < 0) return;

  ReliableSchedule& schedule = reliableSlaveSchedules[selected];
  schedule.lastRunDateKey = dateKey;
  schedule.lastRunMinuteOfDay = minuteOfDay;
  reliableSaveSlaveSchedules();
  reliableStartSlaveRelay(
      schedule.localRelay - 1,
      (uint32_t)schedule.runMinutes * 60UL,
      "catchup",
      selected);
  slaveTimeStatus = String("clock recovered; caught up Zone ") +
      schedule.logicalChannel + " after " + selectedLag + " minutes";
}

static void slaveCatchupTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (meshIsSlave() && meshLegacyNeutralized) {
      bool valid = clockIsValid();
      if (valid && !slaveCatchupPreviousClockValid) {
        slaveCatchupAfterClockRecovery();
      }
      slaveCatchupPreviousClockValid = valid;
    } else {
      // Do not consume the false-to-true clock transition before the Slave
      // runtime has disabled legacy schedules and initialized all six relays.
      slaveCatchupPreviousClockValid = false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void slaveScheduleCatchupPostInit() {
  slaveCatchupPreviousClockValid = false;
  slaveCatchupEvaluatedDateKey = -1;
  // Make the first clock report eligible immediately after the slave reaches
  // the master instead of waiting the normal reporting interval.
  slaveTimeLastStatusReportMs = millis() - SLAVE_TIME_STATUS_REPORT_MS;
  xTaskCreatePinnedToCore(
      slaveCatchupTask,
      "slaveCatchup",
      6144,
      nullptr,
      1,
      &slaveCatchupTaskHandle,
      0);
}
