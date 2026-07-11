#include <sys/time.h>
#include <limits.h>
#include <stdlib.h>

// Reliable slave scheduling layer.
//
// The master remains the configuration authority, but it does not execute or
// assume slave runs. Each slave persists the schedule subset assigned to its
// six relays, executes it from its local clock, and reports immutable run
// events plus heartbeat-confirmed relay state back to the master.

static const char RELIABLE_MASTER_PREF_NAMESPACE[] = "r6masterdist";
static const char RELIABLE_SLAVE_PREF_NAMESPACE[] = "r6slavesched";
static const char RELIABLE_RUN_PREF_NAMESPACE[] = "r6runlog";
static const char RELIABLE_OBS_PREF_NAMESPACE[] = "r6observed";
static const uint8_t RELIABLE_RUN_EVENT_CAPACITY = 32;
static const uint8_t RELIABLE_RUN_EVENT_START = 1;
static const uint8_t RELIABLE_RUN_EVENT_STOP = 2;

struct ReliableSchedule {
  bool enabled;
  uint8_t logicalChannel;
  uint8_t localRelay;
  uint8_t startHour;
  uint8_t startMinute;
  uint16_t runMinutes;
  uint8_t daysMask;
  int lastRunDateKey;
  int lastRunMinuteOfDay;
};

struct ReliableRunEvent {
  uint32_t sequence;
  uint8_t eventType;
  uint32_t runSequence;
  uint8_t relay;
  uint8_t logicalChannel;
  int16_t scheduleIndex;
  uint32_t epoch;
  uint32_t durationSeconds;
  char source[12];
  char outcome[12];
};

struct ReliableObservedRun {
  uint32_t lastEventSequence;
  uint32_t runSequence;
  uint32_t startedEpoch;
  uint32_t durationSeconds;
  uint32_t endedEpoch;
  bool active;
  int16_t scheduleIndex;
  char source[12];
  char outcome[12];
};

static ReliableSchedule reliableMasterSchedules[MESH_MAX_REMOTE_SCHEDULES];
static uint8_t reliableMasterScheduleCount = 0;
static uint32_t reliableMasterScheduleRevision = 0;

static ReliableSchedule reliableSlaveSchedules[MESH_MAX_REMOTE_SCHEDULES];
static uint8_t reliableSlaveScheduleCount = 0;
static uint32_t reliableSlaveScheduleRevision = 0;

static ReliableRunEvent reliableRunEvents[RELIABLE_RUN_EVENT_CAPACITY];
static uint8_t reliableRunEventCount = 0;
static uint32_t reliableNextRunSequence = 1;
static uint32_t reliableActiveRunSequence[MESH_RELAYS_PER_SLAVE] = {0, 0, 0, 0, 0, 0};
static uint32_t reliableActiveStartedEpoch[MESH_RELAYS_PER_SLAVE] = {0, 0, 0, 0, 0, 0};
static uint32_t reliableActiveDurationSeconds[MESH_RELAYS_PER_SLAVE] = {0, 0, 0, 0, 0, 0};
static int16_t reliableActiveScheduleIndex[MESH_RELAYS_PER_SLAVE] = {-1, -1, -1, -1, -1, -1};
static char reliableActiveSource[MESH_RELAYS_PER_SLAVE][12];

static uint32_t reliableSlaveAppliedRevision[MESH_MAX_SLAVES] = {0, 0, 0, 0};
static uint32_t reliableSlaveAckRunSequence[MESH_MAX_SLAVES] = {0, 0, 0, 0};
static ReliableObservedRun reliableObservedRuns[MESH_MAX_SLAVES][MESH_RELAYS_PER_SLAVE];

static bool reliableObservedRelayOn[MESH_RELAYS_PER_SLAVE] = {false, false, false, false, false, false};
static uint32_t reliableLastHeartbeatMs = 0;
static TaskHandle_t reliableTaskHandle = nullptr;

static uint32_t reliableNowEpoch() {
  time_t now = time(nullptr);
  return now > 1700000000UL ? (uint32_t)now : 0;
}

static void reliableSyncClock(uint32_t masterEpoch) {
  if (masterEpoch <= 1700000000UL) return;
  time_t localNow = time(nullptr);
  long delta = localNow > 0 ? labs((long)localNow - (long)masterEpoch) : LONG_MAX;
  if (delta <= 2) return;
  struct timeval tv;
  tv.tv_sec = masterEpoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

static uint32_t reliableScheduleSignature(const ReliableSchedule& schedule) {
  uint32_t hash = 2166136261UL;
  const uint32_t values[] = {
    (uint32_t)(schedule.enabled ? 1U : 0U),
    schedule.logicalChannel,
    schedule.localRelay,
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

static void reliableClearObservedRuns() {
  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    reliableSlaveAppliedRevision[slot] = 0;
    reliableSlaveAckRunSequence[slot] = 0;
    for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
      ReliableObservedRun& run = reliableObservedRuns[slot][relay];
      run.lastEventSequence = 0;
      run.runSequence = 0;
      run.startedEpoch = 0;
      run.durationSeconds = 0;
      run.endedEpoch = 0;
      run.active = false;
      run.scheduleIndex = -1;
      run.source[0] = 0;
      run.outcome[0] = 0;
    }
  }
}

static void reliableSaveMasterSchedules() {
  Preferences p;
  if (!p.begin(RELIABLE_MASTER_PREF_NAMESPACE, false)) return;
  p.putUInt("revision", reliableMasterScheduleRevision);
  p.putUChar("count", reliableMasterScheduleCount);
  for (uint8_t i = 0; i < reliableMasterScheduleCount; i++) {
    String key = "m" + String(i);
    ReliableSchedule& schedule = reliableMasterSchedules[i];
    p.putBool((key + "en").c_str(), schedule.enabled);
    p.putUChar((key + "ch").c_str(), schedule.logicalChannel);
    p.putUChar((key + "lr").c_str(), schedule.localRelay);
    p.putUChar((key + "h").c_str(), schedule.startHour);
    p.putUChar((key + "m").c_str(), schedule.startMinute);
    p.putUShort((key + "dur").c_str(), schedule.runMinutes);
    p.putUChar((key + "mask").c_str(), schedule.daysMask & 0x7F);
  }
  p.end();
}

static void reliableLoadMasterSchedules() {
  reliableMasterScheduleCount = 0;
  reliableMasterScheduleRevision = 0;
  Preferences p;
  if (!p.begin(RELIABLE_MASTER_PREF_NAMESPACE, true)) return;
  reliableMasterScheduleRevision = p.getUInt("revision", 0);
  uint8_t count = min((uint8_t)MESH_MAX_REMOTE_SCHEDULES, p.getUChar("count", 0));
  for (uint8_t i = 0; i < count; i++) {
    String key = "m" + String(i);
    ReliableSchedule schedule;
    schedule.enabled = p.getBool((key + "en").c_str(), true);
    schedule.logicalChannel = p.getUChar((key + "ch").c_str(), 0);
    schedule.localRelay = p.getUChar((key + "lr").c_str(), 0);
    schedule.startHour = p.getUChar((key + "h").c_str(), 0);
    schedule.startMinute = p.getUChar((key + "m").c_str(), 0);
    schedule.runMinutes = p.getUShort((key + "dur").c_str(), 0);
    schedule.daysMask = p.getUChar((key + "mask").c_str(), 0x7F) & 0x7F;
    schedule.lastRunDateKey = -1;
    schedule.lastRunMinuteOfDay = -1;
    uint8_t slot = 0;
    uint8_t relayIndex = 0;
    if (!meshDecodeLogicalChannel(schedule.logicalChannel, slot, relayIndex)) continue;
    if (schedule.localRelay < 1 || schedule.localRelay > MESH_RELAYS_PER_SLAVE) {
      schedule.localRelay = relayIndex + 1;
    }
    if (schedule.startHour > 23 || schedule.startMinute > 59 || schedule.runMinutes == 0) continue;
    reliableMasterSchedules[reliableMasterScheduleCount++] = schedule;
  }
  p.end();
}

static void reliableSaveSlaveSchedules() {
  Preferences p;
  if (!p.begin(RELIABLE_SLAVE_PREF_NAMESPACE, false)) return;
  p.putUInt("revision", reliableSlaveScheduleRevision);
  p.putUChar("count", reliableSlaveScheduleCount);
  p.putInt("slot", meshAssignedSlot);
  p.putInt("base", meshAssignedBaseChannel);
  for (uint8_t i = 0; i < reliableSlaveScheduleCount; i++) {
    String key = "s" + String(i);
    ReliableSchedule& schedule = reliableSlaveSchedules[i];
    p.putBool((key + "en").c_str(), schedule.enabled);
    p.putUChar((key + "ch").c_str(), schedule.logicalChannel);
    p.putUChar((key + "lr").c_str(), schedule.localRelay);
    p.putUChar((key + "h").c_str(), schedule.startHour);
    p.putUChar((key + "m").c_str(), schedule.startMinute);
    p.putUShort((key + "dur").c_str(), schedule.runMinutes);
    p.putUChar((key + "mask").c_str(), schedule.daysMask & 0x7F);
    p.putInt((key + "date").c_str(), schedule.lastRunDateKey);
    p.putInt((key + "min").c_str(), schedule.lastRunMinuteOfDay);
  }
  p.end();
}

static void reliableLoadSlaveSchedules() {
  reliableSlaveScheduleCount = 0;
  reliableSlaveScheduleRevision = 0;
  Preferences p;
  if (!p.begin(RELIABLE_SLAVE_PREF_NAMESPACE, true)) return;
  reliableSlaveScheduleRevision = p.getUInt("revision", 0);
  meshAssignedSlot = p.getInt("slot", -1);
  meshAssignedBaseChannel = p.getInt("base", -1);
  uint8_t count = min((uint8_t)MESH_MAX_REMOTE_SCHEDULES, p.getUChar("count", 0));
  for (uint8_t i = 0; i < count; i++) {
    String key = "s" + String(i);
    ReliableSchedule schedule;
    schedule.enabled = p.getBool((key + "en").c_str(), true);
    schedule.logicalChannel = p.getUChar((key + "ch").c_str(), 0);
    schedule.localRelay = p.getUChar((key + "lr").c_str(), 0);
    schedule.startHour = p.getUChar((key + "h").c_str(), 0);
    schedule.startMinute = p.getUChar((key + "m").c_str(), 0);
    schedule.runMinutes = p.getUShort((key + "dur").c_str(), 0);
    schedule.daysMask = p.getUChar((key + "mask").c_str(), 0x7F) & 0x7F;
    schedule.lastRunDateKey = p.getInt((key + "date").c_str(), -1);
    schedule.lastRunMinuteOfDay = p.getInt((key + "min").c_str(), -1);
    if (schedule.localRelay < 1 || schedule.localRelay > MESH_RELAYS_PER_SLAVE) continue;
    if (schedule.startHour > 23 || schedule.startMinute > 59 || schedule.runMinutes == 0) continue;
    reliableSlaveSchedules[reliableSlaveScheduleCount++] = schedule;
  }
  p.end();
}

static void reliableSaveRunLog() {
  Preferences p;
  if (!p.begin(RELIABLE_RUN_PREF_NAMESPACE, false)) return;
  p.putUInt("next", reliableNextRunSequence);
  p.putUChar("count", reliableRunEventCount);
  for (uint8_t i = 0; i < reliableRunEventCount; i++) {
    String key = "e" + String(i);
    ReliableRunEvent& event = reliableRunEvents[i];
    p.putUInt((key + "seq").c_str(), event.sequence);
    p.putUChar((key + "type").c_str(), event.eventType);
    p.putUInt((key + "run").c_str(), event.runSequence);
    p.putUChar((key + "relay").c_str(), event.relay);
    p.putUChar((key + "ch").c_str(), event.logicalChannel);
    p.putInt((key + "sid").c_str(), event.scheduleIndex);
    p.putUInt((key + "time").c_str(), event.epoch);
    p.putUInt((key + "dur").c_str(), event.durationSeconds);
    p.putString((key + "src").c_str(), event.source);
    p.putString((key + "out").c_str(), event.outcome);
  }
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    String key = "a" + String(relay);
    p.putUInt((key + "run").c_str(), reliableActiveRunSequence[relay]);
    p.putUInt((key + "time").c_str(), reliableActiveStartedEpoch[relay]);
    p.putUInt((key + "dur").c_str(), reliableActiveDurationSeconds[relay]);
    p.putInt((key + "sid").c_str(), reliableActiveScheduleIndex[relay]);
    p.putString((key + "src").c_str(), reliableActiveSource[relay]);
  }
  p.end();
}

static void reliableLoadRunLog() {
  reliableRunEventCount = 0;
  reliableNextRunSequence = 1;
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    reliableActiveRunSequence[relay] = 0;
    reliableActiveStartedEpoch[relay] = 0;
    reliableActiveDurationSeconds[relay] = 0;
    reliableActiveScheduleIndex[relay] = -1;
    reliableActiveSource[relay][0] = 0;
  }

  Preferences p;
  if (!p.begin(RELIABLE_RUN_PREF_NAMESPACE, true)) return;
  reliableNextRunSequence = p.getUInt("next", 1);
  if (reliableNextRunSequence == 0) reliableNextRunSequence = 1;
  reliableRunEventCount = min((uint8_t)RELIABLE_RUN_EVENT_CAPACITY, p.getUChar("count", 0));
  for (uint8_t i = 0; i < reliableRunEventCount; i++) {
    String key = "e" + String(i);
    ReliableRunEvent& event = reliableRunEvents[i];
    event.sequence = p.getUInt((key + "seq").c_str(), 0);
    event.eventType = p.getUChar((key + "type").c_str(), 0);
    event.runSequence = p.getUInt((key + "run").c_str(), 0);
    event.relay = p.getUChar((key + "relay").c_str(), 0);
    event.logicalChannel = p.getUChar((key + "ch").c_str(), 0);
    event.scheduleIndex = p.getInt((key + "sid").c_str(), -1);
    event.epoch = p.getUInt((key + "time").c_str(), 0);
    event.durationSeconds = p.getUInt((key + "dur").c_str(), 0);
    String value = p.getString((key + "src").c_str(), "");
    strlcpy(event.source, value.c_str(), sizeof(event.source));
    value = p.getString((key + "out").c_str(), "");
    strlcpy(event.outcome, value.c_str(), sizeof(event.outcome));
  }
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    String key = "a" + String(relay);
    reliableActiveRunSequence[relay] = p.getUInt((key + "run").c_str(), 0);
    reliableActiveStartedEpoch[relay] = p.getUInt((key + "time").c_str(), 0);
    reliableActiveDurationSeconds[relay] = p.getUInt((key + "dur").c_str(), 0);
    reliableActiveScheduleIndex[relay] = p.getInt((key + "sid").c_str(), -1);
    String value = p.getString((key + "src").c_str(), "");
    strlcpy(reliableActiveSource[relay], value.c_str(), sizeof(reliableActiveSource[relay]));
  }
  p.end();
}

static void reliableSaveObservations() {
  Preferences p;
  if (!p.begin(RELIABLE_OBS_PREF_NAMESPACE, false)) return;
  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    String sk = "s" + String(slot);
    p.putUInt((sk + "rev").c_str(), reliableSlaveAppliedRevision[slot]);
    p.putUInt((sk + "ack").c_str(), reliableSlaveAckRunSequence[slot]);
    for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
      String key = "s" + String(slot) + "r" + String(relay);
      ReliableObservedRun& run = reliableObservedRuns[slot][relay];
      p.putUInt((key + "evt").c_str(), run.lastEventSequence);
      p.putUInt((key + "run").c_str(), run.runSequence);
      p.putUInt((key + "st").c_str(), run.startedEpoch);
      p.putUInt((key + "dur").c_str(), run.durationSeconds);
      p.putUInt((key + "end").c_str(), run.endedEpoch);
      p.putBool((key + "act").c_str(), run.active);
      p.putInt((key + "sid").c_str(), run.scheduleIndex);
      p.putString((key + "src").c_str(), run.source);
      p.putString((key + "out").c_str(), run.outcome);
    }
  }
  p.end();
}

static void reliableLoadObservations() {
  reliableClearObservedRuns();
  Preferences p;
  if (!p.begin(RELIABLE_OBS_PREF_NAMESPACE, true)) return;
  for (uint8_t slot = 0; slot < MESH_MAX_SLAVES; slot++) {
    String sk = "s" + String(slot);
    reliableSlaveAppliedRevision[slot] = p.getUInt((sk + "rev").c_str(), 0);
    reliableSlaveAckRunSequence[slot] = p.getUInt((sk + "ack").c_str(), 0);
    for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
      String key = "s" + String(slot) + "r" + String(relay);
      ReliableObservedRun& run = reliableObservedRuns[slot][relay];
      run.lastEventSequence = p.getUInt((key + "evt").c_str(), 0);
      run.runSequence = p.getUInt((key + "run").c_str(), 0);
      run.startedEpoch = p.getUInt((key + "st").c_str(), 0);
      run.durationSeconds = p.getUInt((key + "dur").c_str(), 0);
      run.endedEpoch = p.getUInt((key + "end").c_str(), 0);
      run.active = p.getBool((key + "act").c_str(), false);
      run.scheduleIndex = p.getInt((key + "sid").c_str(), -1);
      String value = p.getString((key + "src").c_str(), "");
      strlcpy(run.source, value.c_str(), sizeof(run.source));
      value = p.getString((key + "out").c_str(), "");
      strlcpy(run.outcome, value.c_str(), sizeof(run.outcome));
    }
  }
  p.end();
}

static uint32_t reliableAllocateSequence() {
  uint32_t sequence = reliableNextRunSequence++;
  if (sequence == 0) sequence = reliableNextRunSequence++;
  return sequence;
}

static void reliableAppendEvent(const ReliableRunEvent& event) {
  if (reliableRunEventCount >= RELIABLE_RUN_EVENT_CAPACITY) {
    for (uint8_t i = 1; i < reliableRunEventCount; i++) {
      reliableRunEvents[i - 1] = reliableRunEvents[i];
    }
    reliableRunEventCount--;
  }
  reliableRunEvents[reliableRunEventCount++] = event;
}

static uint8_t reliableLogicalChannelForRelay(uint8_t relayIndex) {
  if (meshAssignedBaseChannel >= MESH_FIRST_LOGICAL_CHANNEL) {
    return (uint8_t)(meshAssignedBaseChannel + relayIndex);
  }
  return (uint8_t)(MESH_FIRST_LOGICAL_CHANNEL + relayIndex);
}

static void reliableAppendStopEvent(uint8_t relayIndex, const char* outcome) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE || reliableActiveRunSequence[relayIndex] == 0) return;
  ReliableRunEvent event;
  memset(&event, 0, sizeof(event));
  event.sequence = reliableAllocateSequence();
  event.eventType = RELIABLE_RUN_EVENT_STOP;
  event.runSequence = reliableActiveRunSequence[relayIndex];
  event.relay = relayIndex + 1;
  event.logicalChannel = reliableLogicalChannelForRelay(relayIndex);
  event.scheduleIndex = reliableActiveScheduleIndex[relayIndex];
  event.epoch = reliableNowEpoch();
  event.durationSeconds = reliableActiveDurationSeconds[relayIndex];
  strlcpy(event.source, reliableActiveSource[relayIndex], sizeof(event.source));
  strlcpy(event.outcome, outcome ? outcome : "stopped", sizeof(event.outcome));
  reliableAppendEvent(event);

  reliableActiveRunSequence[relayIndex] = 0;
  reliableActiveStartedEpoch[relayIndex] = 0;
  reliableActiveDurationSeconds[relayIndex] = 0;
  reliableActiveScheduleIndex[relayIndex] = -1;
  reliableActiveSource[relayIndex][0] = 0;
}

static void reliableStartSlaveRelay(
    uint8_t relayIndex,
    uint32_t durationSeconds,
    const char* source,
    int16_t scheduleIndex) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE || durationSeconds == 0) return;

  MeshGuard guard;
  if (!guard.acquired) return;

  if (reliableActiveRunSequence[relayIndex] != 0) {
    reliableAppendStopEvent(relayIndex, "replaced");
  }

  ReliableRunEvent event;
  memset(&event, 0, sizeof(event));
  event.sequence = reliableAllocateSequence();
  event.eventType = RELIABLE_RUN_EVENT_START;
  event.runSequence = event.sequence;
  event.relay = relayIndex + 1;
  event.logicalChannel = reliableLogicalChannelForRelay(relayIndex);
  event.scheduleIndex = scheduleIndex;
  event.epoch = reliableNowEpoch();
  event.durationSeconds = constrain(durationSeconds, (uint32_t)1, (uint32_t)(240UL * 60UL));
  strlcpy(event.source, source ? source : "manual", sizeof(event.source));
  event.outcome[0] = 0;
  reliableAppendEvent(event);

  reliableActiveRunSequence[relayIndex] = event.runSequence;
  reliableActiveStartedEpoch[relayIndex] = event.epoch;
  reliableActiveDurationSeconds[relayIndex] = event.durationSeconds;
  reliableActiveScheduleIndex[relayIndex] = scheduleIndex;
  strlcpy(reliableActiveSource[relayIndex], event.source, sizeof(reliableActiveSource[relayIndex]));

  meshStartSlaveRelay(relayIndex, event.durationSeconds);
  reliableObservedRelayOn[relayIndex] = true;
  reliableSaveRunLog();
}

static void reliableStopSlaveRelay(uint8_t relayIndex, const char* outcome) {
  if (relayIndex >= MESH_RELAYS_PER_SLAVE) return;
  MeshGuard guard;
  if (!guard.acquired) return;
  meshStopSlaveRelay(relayIndex);
  reliableAppendStopEvent(relayIndex, outcome);
  reliableObservedRelayOn[relayIndex] = false;
  reliableSaveRunLog();
}

static void reliableStopAllSlaveRelays(const char* outcome) {
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    reliableStopSlaveRelay(relay, outcome);
  }
}

static void reliableObserveRelayTransitions() {
  MeshGuard guard;
  if (!guard.acquired) return;
  bool changed = false;
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    bool on = relayState[relay];
    if (reliableObservedRelayOn[relay] && !on) {
      reliableAppendStopEvent(relay, "completed");
      changed = true;
    } else if (!reliableObservedRelayOn[relay] && on && reliableActiveRunSequence[relay] == 0) {
      ReliableRunEvent event;
      memset(&event, 0, sizeof(event));
      event.sequence = reliableAllocateSequence();
      event.eventType = RELIABLE_RUN_EVENT_START;
      event.runSequence = event.sequence;
      event.relay = relay + 1;
      event.logicalChannel = reliableLogicalChannelForRelay(relay);
      event.scheduleIndex = -1;
      event.epoch = reliableNowEpoch();
      event.durationSeconds = meshSlaveRemainingSeconds(relay);
      strlcpy(event.source, "external", sizeof(event.source));
      reliableAppendEvent(event);
      reliableActiveRunSequence[relay] = event.runSequence;
      reliableActiveStartedEpoch[relay] = event.epoch;
      reliableActiveDurationSeconds[relay] = event.durationSeconds;
      reliableActiveScheduleIndex[relay] = -1;
      strlcpy(reliableActiveSource[relay], event.source, sizeof(reliableActiveSource[relay]));
      changed = true;
    }
    reliableObservedRelayOn[relay] = on;
  }
  if (changed) reliableSaveRunLog();
}

static void reliableAcknowledgeRunEvents(uint32_t acknowledgedSequence) {
  if (acknowledgedSequence == 0) return;
  MeshGuard guard;
  if (!guard.acquired) return;
  uint8_t writeIndex = 0;
  for (uint8_t i = 0; i < reliableRunEventCount; i++) {
    if (reliableRunEvents[i].sequence <= acknowledgedSequence) continue;
    if (writeIndex != i) reliableRunEvents[writeIndex] = reliableRunEvents[i];
    writeIndex++;
  }
  if (writeIndex != reliableRunEventCount) {
    reliableRunEventCount = writeIndex;
    reliableSaveRunLog();
  }
}

static void reliableCloseInterruptedRunsAfterBoot() {
  MeshGuard guard;
  if (!guard.acquired) return;
  bool changed = false;
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    reliableObservedRelayOn[relay] = relayState[relay];
    if (reliableActiveRunSequence[relay] != 0 && !relayState[relay]) {
      reliableAppendStopEvent(relay, "reboot");
      changed = true;
    }
  }
  if (changed) reliableSaveRunLog();
}

static bool reliableValidateMasterSchedules(JsonArray schedules, String& errorOut) {
  if (schedules.size() > MESH_MAX_REMOTE_SCHEDULES) {
    errorOut = "too many slave-zone schedules";
    return false;
  }
  if (dailyScheduleCount + spigotScheduleCount + schedules.size() > MAX_DAILY_SCHEDULES) {
    errorOut = "combined local and slave schedule limit is 64";
    return false;
  }

  struct Interval {
    int start;
    int end;
    uint8_t days;
  } intervals[MESH_MAX_REMOTE_SCHEDULES + MAX_DAILY_SCHEDULES];
  uint8_t intervalCount = 0;

  for (uint8_t i = 0; i < dailyScheduleCount; i++) {
    if (!dailySchedules[i].enabled) continue;
    intervals[intervalCount++] = {
      dailySchedules[i].startHour * 60 + dailySchedules[i].startMinute,
      dailySchedules[i].startHour * 60 + dailySchedules[i].startMinute + dailySchedules[i].runMinutes,
      i < trackedScheduleCount ? scheduleDaysMasks[i] : (uint8_t)0x7F
    };
  }
  for (uint8_t i = 0; i < spigotScheduleCount; i++) {
    if (!spigotSchedules[i].enabled) continue;
    intervals[intervalCount++] = {
      spigotSchedules[i].startHour * 60 + spigotSchedules[i].startMinute,
      spigotSchedules[i].startHour * 60 + spigotSchedules[i].startMinute + spigotSchedules[i].runMinutes,
      spigotSchedules[i].daysMask
    };
  }

  ReliableSchedule proposed[MESH_MAX_REMOTE_SCHEDULES];
  uint8_t proposedCount = 0;

  for (JsonObject item : schedules) {
    int channel = item["logicalChannel"] | (item["channel"] | 0);
    String start = String((const char*)(item["startTime"] | ""));
    int colon = start.indexOf(':');
    int hour = colon > 0 ? start.substring(0, colon).toInt() : -1;
    int minute = colon > 0 ? start.substring(colon + 1).toInt() : -1;
    int seconds = item["durationSeconds"] | 0;
    int runMinutes = (seconds + 59) / 60;
    bool enabled = item["enabled"].is<bool>() ? (bool)item["enabled"] : true;
    uint8_t daysMask = item["daysMask"].isNull()
        ? 0x7F
        : (uint8_t)(item["daysMask"].as<int>() & 0x7F);

    uint8_t slot = 0;
    uint8_t relay = 0;
    if (!meshDecodeLogicalChannel(channel, slot, relay) || !meshSlaves[slot].assigned) {
      errorOut = String("unknown slave zone ") + channel;
      return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        runMinutes < 1 || runMinutes > 240) {
      errorOut = "invalid slave-zone schedule row";
      return false;
    }

    int startMinutes = hour * 60 + minute;
    int endMinutes = startMinutes + runMinutes;
    if (enabled && (startMinutes < 4 * 60 || endMinutes > 20 * 60)) {
      errorOut = "enabled schedules must stay between 04:00 and 20:00";
      return false;
    }

    if (enabled) {
      for (uint8_t i = 0; i < intervalCount; i++) {
        if ((daysMask & intervals[i].days) != 0 &&
            startMinutes < intervals[i].end &&
            intervals[i].start < endMinutes) {
          errorOut = "slave-zone schedule overlaps another enabled watering run on at least one selected day";
          return false;
        }
      }
      intervals[intervalCount++] = {startMinutes, endMinutes, daysMask};
    }

    ReliableSchedule& schedule = proposed[proposedCount++];
    schedule.enabled = enabled;
    schedule.logicalChannel = (uint8_t)channel;
    schedule.localRelay = relay + 1;
    schedule.startHour = hour;
    schedule.startMinute = minute;
    schedule.runMinutes = runMinutes;
    schedule.daysMask = daysMask;
    schedule.lastRunDateKey = -1;
    schedule.lastRunMinuteOfDay = -1;
  }

  MeshGuard guard;
  if (!guard.acquired) {
    errorOut = "schedule storage busy";
    return false;
  }

  reliableMasterScheduleCount = proposedCount;
  for (uint8_t i = 0; i < proposedCount; i++) {
    reliableMasterSchedules[i] = proposed[i];
  }
  reliableMasterScheduleRevision++;
  if (reliableMasterScheduleRevision == 0) reliableMasterScheduleRevision = 1;
  reliableSaveMasterSchedules();
  return true;
}

static void reliableAddMasterSchedules(JsonArray target) {
  MeshGuard guard;
  if (!guard.acquired) return;
  for (uint8_t i = 0; i < reliableMasterScheduleCount; i++) {
    ReliableSchedule& schedule = reliableMasterSchedules[i];
    JsonObject item = target.createNestedObject();
    item["id"] = i;
    item["logicalChannel"] = schedule.logicalChannel;
    item["channel"] = schedule.logicalChannel;
    item["zone"] = meshLogicalZoneLabel(schedule.logicalChannel);
    item["enabled"] = schedule.enabled;
    item["startTime"] = htmlTimeValue(schedule.startHour, schedule.startMinute);
    item["durationSeconds"] = (uint32_t)schedule.runMinutes * 60UL;
    item["daysMask"] = schedule.daysMask;
    item["daysLabel"] = scheduleDaysMaskLabel(schedule.daysMask);
  }
}

static void reliableAddSchedulesForSlot(JsonArray target, uint8_t slot) {
  MeshGuard guard;
  if (!guard.acquired) return;
  for (uint8_t i = 0; i < reliableMasterScheduleCount; i++) {
    ReliableSchedule& schedule = reliableMasterSchedules[i];
    uint8_t scheduleSlot = 0;
    uint8_t relay = 0;
    if (!meshDecodeLogicalChannel(schedule.logicalChannel, scheduleSlot, relay) ||
        scheduleSlot != slot) {
      continue;
    }
    JsonObject item = target.createNestedObject();
    item["id"] = i;
    item["logicalChannel"] = schedule.logicalChannel;
    item["localRelay"] = relay + 1;
    item["enabled"] = schedule.enabled;
    item["startTime"] = htmlTimeValue(schedule.startHour, schedule.startMinute);
    item["durationSeconds"] = (uint32_t)schedule.runMinutes * 60UL;
    item["daysMask"] = schedule.daysMask;
  }
}

static bool reliableApplySlaveScheduleSnapshot(
    uint32_t revision,
    JsonArray schedules,
    String& errorOut) {
  if (schedules.size() > MESH_MAX_REMOTE_SCHEDULES) {
    errorOut = "too many schedules in master snapshot";
    return false;
  }

  ReliableSchedule proposed[MESH_MAX_REMOTE_SCHEDULES];
  uint8_t proposedCount = 0;

  for (JsonObject item : schedules) {
    int logicalChannel = item["logicalChannel"] | 0;
    int localRelay = item["localRelay"] | 0;
    String start = String((const char*)(item["startTime"] | ""));
    int colon = start.indexOf(':');
    int hour = colon > 0 ? start.substring(0, colon).toInt() : -1;
    int minute = colon > 0 ? start.substring(colon + 1).toInt() : -1;
    int seconds = item["durationSeconds"] | 0;
    int runMinutes = (seconds + 59) / 60;
    if (localRelay < 1 || localRelay > MESH_RELAYS_PER_SLAVE ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        runMinutes < 1 || runMinutes > 240) {
      errorOut = "invalid schedule in master snapshot";
      return false;
    }

    ReliableSchedule& schedule = proposed[proposedCount++];
    schedule.enabled = item["enabled"].is<bool>() ? (bool)item["enabled"] : true;
    schedule.logicalChannel = logicalChannel;
    schedule.localRelay = localRelay;
    schedule.startHour = hour;
    schedule.startMinute = minute;
    schedule.runMinutes = runMinutes;
    schedule.daysMask = item["daysMask"].isNull()
        ? 0x7F
        : (uint8_t)(item["daysMask"].as<int>() & 0x7F);
    schedule.lastRunDateKey = -1;
    schedule.lastRunMinuteOfDay = -1;

    uint32_t signature = reliableScheduleSignature(schedule);
    for (uint8_t oldIndex = 0; oldIndex < reliableSlaveScheduleCount; oldIndex++) {
      if (reliableScheduleSignature(reliableSlaveSchedules[oldIndex]) == signature) {
        schedule.lastRunDateKey = reliableSlaveSchedules[oldIndex].lastRunDateKey;
        schedule.lastRunMinuteOfDay = reliableSlaveSchedules[oldIndex].lastRunMinuteOfDay;
        break;
      }
    }
  }

  MeshGuard guard;
  if (!guard.acquired) {
    errorOut = "slave schedule storage busy";
    return false;
  }
  reliableSlaveScheduleCount = proposedCount;
  for (uint8_t i = 0; i < proposedCount; i++) {
    reliableSlaveSchedules[i] = proposed[i];
  }
  reliableSlaveScheduleRevision = revision;
  reliableSaveSlaveSchedules();
  return true;
}

static void reliableServiceSlaveSchedules() {
  if (!meshIsSlave() || !meshLegacyNeutralized || !clockIsValid()) return;
  struct tm now;
  if (!getLocalTime(&now, 20)) return;
  int minuteOfDay = now.tm_hour * 60 + now.tm_min;
  int dateKey = (now.tm_year + 1900) * 512 + now.tm_yday;

  bool changed = false;
  for (uint8_t i = 0; i < reliableSlaveScheduleCount; i++) {
    ReliableSchedule& schedule = reliableSlaveSchedules[i];
    if (!schedule.enabled || schedule.runMinutes == 0 ||
        schedule.localRelay < 1 || schedule.localRelay > MESH_RELAYS_PER_SLAVE ||
        (schedule.daysMask & (1U << now.tm_wday)) == 0) {
      continue;
    }
    int scheduledMinute = schedule.startHour * 60 + schedule.startMinute;
    if (scheduledMinute != minuteOfDay) continue;
    if (schedule.lastRunDateKey == dateKey &&
        schedule.lastRunMinuteOfDay == minuteOfDay) {
      continue;
    }

    schedule.lastRunDateKey = dateKey;
    schedule.lastRunMinuteOfDay = minuteOfDay;
    reliableStartSlaveRelay(
        schedule.localRelay - 1,
        (uint32_t)schedule.runMinutes * 60UL,
        "schedule",
        i);
    changed = true;
  }
  if (changed) reliableSaveSlaveSchedules();
}

static bool reliableIngestRunEvents(uint8_t slot, JsonArray events) {
  if (slot >= MESH_MAX_SLAVES) return false;
  bool changed = false;
  uint32_t highest = reliableSlaveAckRunSequence[slot];

  for (JsonObject item : events) {
    uint32_t sequence = item["sequence"] | 0;
    uint8_t eventType = item["eventType"] | 0;
    uint32_t runSequence = item["runSequence"] | 0;
    int relay = item["relay"] | 0;
    int logicalChannel = item["logicalChannel"] | 0;
    if (sequence == 0 || runSequence == 0 ||
        relay < 1 || relay > MESH_RELAYS_PER_SLAVE ||
        logicalChannel != meshLogicalChannel(slot, relay - 1) ||
        (eventType != RELIABLE_RUN_EVENT_START &&
         eventType != RELIABLE_RUN_EVENT_STOP)) {
      continue;
    }

    ReliableObservedRun& observed = reliableObservedRuns[slot][relay - 1];
    if (sequence >= observed.lastEventSequence) {
      observed.lastEventSequence = sequence;
      observed.runSequence = runSequence;
      observed.scheduleIndex = item["scheduleIndex"] | -1;
      String source = String((const char*)(item["source"] | ""));
      if (source.length()) strlcpy(observed.source, source.c_str(), sizeof(observed.source));

      if (eventType == RELIABLE_RUN_EVENT_START) {
        observed.startedEpoch = item["epoch"] | 0;
        observed.durationSeconds = item["durationSeconds"] | 0;
        observed.endedEpoch = 0;
        observed.active = true;
        observed.outcome[0] = 0;
      } else {
        observed.endedEpoch = item["epoch"] | 0;
        observed.active = false;
        String outcome = String((const char*)(item["outcome"] | "stopped"));
        strlcpy(observed.outcome, outcome.c_str(), sizeof(observed.outcome));
      }
      changed = true;
    }
    highest = max(highest, sequence);
  }

  if (highest != reliableSlaveAckRunSequence[slot]) {
    reliableSlaveAckRunSequence[slot] = highest;
    changed = true;
  }
  return changed;
}

static void reliableAddRunEventsToHeartbeat(JsonArray target) {
  MeshGuard guard;
  if (!guard.acquired) return;
  for (uint8_t i = 0; i < reliableRunEventCount; i++) {
    ReliableRunEvent& event = reliableRunEvents[i];
    JsonObject item = target.createNestedObject();
    item["sequence"] = event.sequence;
    item["eventType"] = event.eventType;
    item["runSequence"] = event.runSequence;
    item["relay"] = event.relay;
    item["logicalChannel"] = event.logicalChannel;
    item["scheduleIndex"] = event.scheduleIndex;
    item["epoch"] = event.epoch;
    item["durationSeconds"] = event.durationSeconds;
    item["source"] = event.source;
    if (event.eventType == RELIABLE_RUN_EVENT_STOP) item["outcome"] = event.outcome;
  }
}

static void reliableHandleSlaveHeartbeat() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in master mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument request(12288);
  if (deserializeJson(request, server.arg("plain")) || !meshVerifyLinkKey(request)) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid slave registration\"}");
    return;
  }

  const char* id = request["deviceId"] | "";
  const char* name = request["deviceName"] | id;
  int slot = -1;
  bool observationChanged = false;
  bool hasReportedRevision = !request["scheduleRevision"].isNull();
  uint32_t reportedRevision = hasReportedRevision
      ? (uint32_t)(request["scheduleRevision"] | 0)
      : 0;

  {
    MeshGuard guard;
    if (!guard.acquired) {
      server.send(503, "application/json", "{\"ok\":false,\"error\":\"mesh busy\"}");
      return;
    }

    slot = meshFindOrAssignSlave(id, name);
    if (slot < 0) {
      server.send(507, "application/json", "{\"ok\":false,\"error\":\"no slave slots available\"}");
      return;
    }

    MeshSlaveDevice& slave = meshSlaves[slot];
    String remoteIp = server.client().remoteIP().toString();
    strlcpy(slave.ip, remoteIp.c_str(), sizeof(slave.ip));
    slave.lastSeenMs = millis();

    JsonArray relays = request["relays"].as<JsonArray>();
    for (JsonObject relay : relays) {
      int number = relay["relay"] | 0;
      if (number < 1 || number > MESH_RELAYS_PER_SLAVE) continue;
      slave.relayOn[number - 1] = relay["on"] | false;
      slave.remainingSeconds[number - 1] = relay["remainingSeconds"] | 0;
    }

    if (hasReportedRevision &&
        reliableSlaveAppliedRevision[slot] != reportedRevision) {
      reliableSlaveAppliedRevision[slot] = reportedRevision;
      observationChanged = true;
    }

    if (request["runEvents"].is<JsonArray>()) {
      observationChanged |= reliableIngestRunEvents(
          slot,
          request["runEvents"].as<JsonArray>());
    }
  }

  if (observationChanged) reliableSaveObservations();

  DynamicJsonDocument response(16384);
  response["ok"] = true;
  response["slot"] = slot;
  response["baseLogicalChannel"] = meshLogicalChannel(slot, 0);
  response["heartbeatIntervalSeconds"] = MESH_HEARTBEAT_INTERVAL_MS / 1000UL;
  response["masterTime"] = reliableNowEpoch();
  response["scheduleRevision"] = reliableMasterScheduleRevision;
  response["ackRunSequence"] = reliableSlaveAckRunSequence[slot];

  uint32_t effectiveRevision = hasReportedRevision
      ? reportedRevision
      : reliableSlaveAppliedRevision[slot];
  if (effectiveRevision != reliableMasterScheduleRevision) {
    response["scheduleUpdate"] = true;
    JsonArray schedules = response.createNestedArray("schedules");
    reliableAddSchedulesForSlot(schedules, slot);
  } else {
    response["scheduleUpdate"] = false;
  }

  String body;
  serializeJson(response, body);
  server.send(200, "application/json", body);
}

static bool reliableSendHeartbeat() {
  if (!meshIsSlave() || WiFi.status() != WL_CONNECTED) return false;

  DynamicJsonDocument request(12288);
  request["linkKey"] = meshLinkKey;
  request["deviceId"] = meshDeviceId;
  request["deviceName"] = meshDeviceName;
  request["firmwareVersion"] = FIRMWARE_VERSION;
  request["relayCount"] = MESH_RELAYS_PER_SLAVE;
  request["stationIp"] = WiFi.localIP().toString();
  request["scheduleRevision"] = reliableSlaveScheduleRevision;

  JsonArray relays = request.createNestedArray("relays");
  for (uint8_t relay = 0; relay < MESH_RELAYS_PER_SLAVE; relay++) {
    JsonObject item = relays.createNestedObject();
    item["relay"] = relay + 1;
    item["on"] = relayState[relay];
    item["remainingSeconds"] = meshSlaveRemainingSeconds(relay);
  }

  JsonArray events = request.createNestedArray("runEvents");
  reliableAddRunEventsToHeartbeat(events);

  String requestBody;
  serializeJson(request, requestBody);
  String responseBody;
  int code = -1;
  bool ok = meshPostJson(
      meshMasterBaseUrl() + "/api/slaves/heartbeat",
      requestBody,
      responseBody,
      code);
  if (!ok) {
    meshLastStatus = String("master heartbeat failed: ") + code;
    return false;
  }

  DynamicJsonDocument response(16384);
  if (deserializeJson(response, responseBody)) {
    meshLastStatus = "master heartbeat response invalid";
    return false;
  }

  int previousSlot = meshAssignedSlot;
  int previousBaseChannel = meshAssignedBaseChannel;
  meshAssignedSlot = response["slot"] | -1;
  meshAssignedBaseChannel = response["baseLogicalChannel"] | -1;
  bool assignmentChanged =
      previousSlot != meshAssignedSlot ||
      previousBaseChannel != meshAssignedBaseChannel;
  meshLastMasterContactMs = millis();
  reliableSyncClock(response["masterTime"] | 0);

  uint32_t targetRevision = response["scheduleRevision"] | reliableSlaveScheduleRevision;
  if ((response["scheduleUpdate"] | false) &&
      response["schedules"].is<JsonArray>()) {
    String error;
    if (!reliableApplySlaveScheduleSnapshot(
            targetRevision,
            response["schedules"].as<JsonArray>(),
            error)) {
      meshLastStatus = "schedule update rejected: " + error;
      return false;
    }
  } else if (assignmentChanged) {
    reliableSaveSlaveSchedules();
  }

  reliableAcknowledgeRunEvents(response["ackRunSequence"] | 0);
  meshLastStatus = String("connected to master as zones ") +
      meshAssignedBaseChannel + "-" + (meshAssignedBaseChannel + 5) +
      "; schedule revision " + reliableSlaveScheduleRevision;
  return true;
}

static void reliableHandleSchedulesGet() {
  DynamicJsonDocument doc(16384);
  doc["ok"] = true;
  doc["revision"] = reliableMasterScheduleRevision;
  JsonArray schedules = doc.createNestedArray("schedules");
  reliableAddMasterSchedules(schedules);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void reliableHandleSchedulesPost() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"slave schedules are synchronized from the master\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, server.arg("plain")) ||
      !doc["schedules"].is<JsonArray>()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json or missing schedules\"}");
    return;
  }

  String error;
  if (!reliableValidateMasterSchedules(doc["schedules"].as<JsonArray>(), error)) {
    DynamicJsonDocument reply(512);
    reply["ok"] = false;
    reply["error"] = error;
    String body;
    serializeJson(reply, body);
    server.send(400, "application/json", body);
    return;
  }

  DynamicJsonDocument reply(512);
  reply["ok"] = true;
  reply["revision"] = reliableMasterScheduleRevision;
  reply["distribution"] = "pending slave heartbeat acknowledgement";
  String body;
  serializeJson(reply, body);
  server.send(200, "application/json", body);
}

static void reliableHandleSlaveRelayCommand() {
  if (!meshIsSlave()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in slave mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain")) || !meshVerifyLinkKey(doc)) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid master command\"}");
    return;
  }

  int relay = doc["relay"] | 0;
  String action = String((const char*)(doc["action"] | ""));
  uint32_t durationSeconds = doc["durationSeconds"] | DEFAULT_MANUAL_RUN_SECONDS;
  if (relay < 1 || relay > MESH_RELAYS_PER_SLAVE ||
      (action != "on" && action != "off" && action != "toggle")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid relay command\"}");
    return;
  }

  uint8_t relayIndex = relay - 1;
  if (action == "off") {
    reliableStopSlaveRelay(relayIndex, "master_stop");
  } else if (action == "toggle" && relayState[relayIndex]) {
    reliableStopSlaveRelay(relayIndex, "master_stop");
  } else {
    reliableStartSlaveRelay(relayIndex, durationSeconds, "master", -1);
  }
  meshLastMasterContactMs = millis();

  DynamicJsonDocument response(512);
  response["ok"] = true;
  response["relay"] = relay;
  response["accepted"] = true;
  response["stateConfirmedBySlave"] = true;
  response["on"] = relayState[relayIndex];
  response["remainingSeconds"] = meshSlaveRemainingSeconds(relayIndex);
  response["runSequence"] = reliableActiveRunSequence[relayIndex];
  String body;
  serializeJson(response, body);
  server.send(200, "application/json", body);
}

static void reliableHandleSlaveAllOff() {
  if (!meshIsSlave()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in slave mode\"}");
    return;
  }
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(384);
    if (deserializeJson(doc, server.arg("plain")) || !meshVerifyLinkKey(doc)) {
      server.send(403, "application/json", "{\"ok\":false,\"error\":\"invalid master command\"}");
      return;
    }
  }
  reliableStopAllSlaveRelays("master_alloff");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void reliableHandleSlaveLocalControl() {
  if (!meshIsSlave() || !server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  int relay = doc["relay"] | 0;
  String action = String((const char*)(doc["action"] | ""));
  uint32_t durationSeconds = doc["durationSeconds"] | DEFAULT_MANUAL_RUN_SECONDS;
  if (relay < 1 || relay > MESH_RELAYS_PER_SLAVE) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"relay must be 1-6\"}");
    return;
  }
  if (action == "off") {
    reliableStopSlaveRelay(relay - 1, "local_stop");
  } else {
    reliableStartSlaveRelay(relay - 1, durationSeconds, "local", -1);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

static void reliableHandleRoleAwareManualRun() {
  if (meshIsMaster()) {
    handleManualRun();
    return;
  }
  int relay = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
  if (relay < 1 || relay > MESH_RELAYS_PER_SLAVE ||
      minutes < 1 || minutes > 240) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"slave relays are 1-6\"}");
    return;
  }
  reliableStartSlaveRelay(relay - 1, (uint32_t)minutes * 60UL, "local", -1);
  server.send(200, "application/json", "{\"ok\":true,\"mode\":\"slave-relay\"}");
}

static void reliableHandleRoleAwareRelay() {
  if (meshIsMaster()) {
    handleRelay();
    return;
  }
  int relay = server.hasArg("zone") ? server.arg("zone").toInt() : 0;
  int state = server.hasArg("state") ? server.arg("state").toInt() : 0;
  if (relay < 1 || relay > MESH_RELAYS_PER_SLAVE) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"slave relays are 1-6\"}");
    return;
  }
  if (state) reliableStartSlaveRelay(relay - 1, DEFAULT_MANUAL_RUN_SECONDS, "local", -1);
  else reliableStopSlaveRelay(relay - 1, "local_stop");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void reliableHandleMasterRelayControl() {
  if (!meshIsMaster()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"device is not in master mode\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"expected json body\"}");
    return;
  }
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  int channel = doc["logicalChannel"] | 0;
  String action = String((const char*)(doc["action"] | ""));
  uint32_t durationSeconds = doc["durationSeconds"] | DEFAULT_MANUAL_RUN_SECONDS;
  if (action != "on" && action != "off" && action != "toggle") {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid action\"}");
    return;
  }

  bool ok = meshSendSlaveCommand(channel, action, durationSeconds);
  DynamicJsonDocument response(512);
  response["ok"] = ok;
  response["acceptedBySlave"] = ok;
  response["statePendingHeartbeat"] = ok;
  if (!ok) response["error"] = "slave unavailable or command failed";
  String body;
  serializeJson(response, body);
  server.send(ok ? 200 : 503, "application/json", body);
}

static void reliableAddLastRunJson(
    JsonObject relayItem,
    const ReliableObservedRun& observed) {
  JsonObject run = relayItem.createNestedObject("lastRun");
  run["known"] = observed.runSequence != 0;
  if (observed.runSequence == 0) return;
  run["runSequence"] = observed.runSequence;
  run["startedEpoch"] = observed.startedEpoch;
  run["durationSeconds"] = observed.durationSeconds;
  run["endedEpoch"] = observed.endedEpoch;
  run["activeByReport"] = observed.active;
  run["scheduleIndex"] = observed.scheduleIndex;
  run["source"] = observed.source;
  run["outcome"] = observed.outcome;
}

static void reliableAddState(JsonDocument& doc) {
  meshAddStateJson(doc);
  doc["reliableSlaveScheduling"] = true;

  if (meshIsSlave()) {
    doc["scheduleRevision"] = reliableSlaveScheduleRevision;
    doc["localScheduleCount"] = reliableSlaveScheduleCount;
    doc["runEventBacklog"] = reliableRunEventCount;
    JsonArray schedules = doc.createNestedArray("localSchedules");
    for (uint8_t i = 0; i < reliableSlaveScheduleCount; i++) {
      ReliableSchedule& schedule = reliableSlaveSchedules[i];
      JsonObject item = schedules.createNestedObject();
      item["logicalChannel"] = schedule.logicalChannel;
      item["localRelay"] = schedule.localRelay;
      item["enabled"] = schedule.enabled;
      item["startTime"] = htmlTimeValue(schedule.startHour, schedule.startMinute);
      item["durationSeconds"] = (uint32_t)schedule.runMinutes * 60UL;
      item["daysMask"] = schedule.daysMask;
    }
    return;
  }

  doc.remove("schedules");
  JsonArray schedules = doc.createNestedArray("schedules");
  reliableAddMasterSchedules(schedules);
  doc["scheduleRevision"] = reliableMasterScheduleRevision;

  JsonArray slaves = doc["slaves"].as<JsonArray>();
  for (JsonObject slaveItem : slaves) {
    int slot = slaveItem["slot"] | -1;
    if (slot < 0 || slot >= MESH_MAX_SLAVES) continue;
    bool online = slaveItem["online"] | false;
    uint32_t ageMs = slaveItem["lastSeenMsAgo"] | 0;
    slaveItem["targetScheduleRevision"] = reliableMasterScheduleRevision;
    slaveItem["appliedScheduleRevision"] = reliableSlaveAppliedRevision[slot];
    slaveItem["scheduleSynchronized"] =
        reliableSlaveAppliedRevision[slot] == reliableMasterScheduleRevision;

    JsonArray relays = slaveItem["relays"].as<JsonArray>();
    for (JsonObject relayItem : relays) {
      int relay = relayItem["relay"] | 0;
      if (relay < 1 || relay > MESH_RELAYS_PER_SLAVE) continue;
      bool lastReportedOn = relayItem["on"] | false;
      uint32_t reportedRemaining = relayItem["remainingSeconds"] | 0;
      relayItem["stateConfirmed"] = online;
      relayItem["reportedMsAgo"] = ageMs;
      relayItem["lastReportedOn"] = lastReportedOn;
      relayItem["reportedRemainingSeconds"] = reportedRemaining;
      relayItem["on"] = online ? lastReportedOn : false;
      if (online && lastReportedOn) {
        uint32_t elapsed = ageMs / 1000UL;
        relayItem["remainingSeconds"] =
            reportedRemaining > elapsed ? reportedRemaining - elapsed : 0;
      } else {
        relayItem["remainingSeconds"] = 0;
      }
      reliableAddLastRunJson(
          relayItem,
          reliableObservedRuns[slot][relay - 1]);
    }
  }
}

static void reliableHandleState() {
  DynamicJsonDocument doc(24576);
  reliableAddState(doc);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void reliableHandleRoleAwareState() {
  if (meshIsMaster()) sendStateJson();
  else reliableHandleState();
}

static String reliableBuildAdminPage() {
  String page = String(FPSTR(MESH_ADMIN_PAGE));
  page.reserve(page.length() + 9000);
  page.replace(
      "Slave schedules are owned and executed by the master.",
      "Slave schedules are configured on the master, synchronized to each slave, persisted there, and executed locally even while disconnected.");
  page.replace(
      "Automatic schedules run only on the master.",
      "This slave stores and executes its synchronized schedules locally. Run state and history are reported to the master when connectivity returns.");
  page.replace(
      "alert('Slave schedules saved');await refreshMesh(true)",
      "alert('Schedules saved on the master. Distribution remains pending until each slave reports the new revision.');await refreshMesh(true)");

  const String originalRefresh =
      "refreshMesh(true);setInterval(()=>refreshMesh(false),2000)";
  const String replacement = R"JS(
function fmtEpoch(v){return Number(v)>0?new Date(Number(v)*1000).toLocaleString():'time unavailable'}
renderMaster=function(s,force){
  e('masterPanels').hidden=false;e('slavePanels').hidden=true;
  e('slaveList').innerHTML=(s.slaves||[]).length?(s.slaves||[]).map(x=>{
    const sync=x.scheduleSynchronized?'Schedules synchronized':`Schedule revision ${x.appliedScheduleRevision||0} / ${x.targetScheduleRevision||0}`;
    return `<div class="card ${x.online?'online':'offline'}"><b>${esc(x.deviceName)}</b><p>${x.online?'Online at '+esc(x.ip):'Offline'} · Zones ${x.baseLogicalChannel}-${x.baseLogicalChannel+5}</p><p>${esc(sync)}</p><small>${esc(x.deviceId)}</small></div>`
  }).join(''):'<p>No slaves have reported yet.</p>';
  e('remoteZones').innerHTML=(s.slaves||[]).flatMap(x=>(x.relays||[]).map(r=>{
    const age=Math.round(Number(r.reportedMsAgo||0)/1000);
    const state=x.online?(r.lastReportedOn?`ON · ${r.remainingSeconds||0}s remaining`:'OFF'):`UNKNOWN · last report ${age}s ago was ${r.lastReportedOn?'ON':'OFF'}`;
    const lr=r.lastRun||{};
    const last=lr.known?`Last actual run: ${esc(lr.source||'unknown')} at ${fmtEpoch(lr.startedEpoch)}${lr.endedEpoch?' · ended '+fmtEpoch(lr.endedEpoch):''}${lr.outcome?' · '+esc(lr.outcome):''}`:'No run has been reported';
    return `<div class="card ${x.online?'online':'offline'}"><b>Zone ${r.logicalChannel}</b><p>${esc(x.deviceName)} relay ${r.relay} · ${state}</p><small>${last}</small><p><button ${x.online?'':'disabled'} onclick="remoteCmd(${r.logicalChannel},'on')">Run</button><button ${x.online?'':'disabled'} onclick="remoteCmd(${r.logicalChannel},'off')">Stop</button></p></div>`
  })).join('');
  if(force){e('meshScheduleRows').innerHTML='';(s.schedules||[]).forEach(addMeshSchedule)}
}
renderSlave=function(s){
  e('masterPanels').hidden=true;e('slavePanels').hidden=false;
  e('slaveRelays').innerHTML=`<div class="card"><b>Local schedule revision ${s.scheduleRevision||0}</b><p>${s.localScheduleCount||0} schedules stored · ${s.runEventBacklog||0} run events awaiting acknowledgement</p></div>`+(s.relays||[]).map(r=>`<div class="card ${r.on?'online':''}"><b>Slave relay ${r.relay}</b><p>${r.on?'ON · '+r.remainingSeconds+'s remaining':'OFF'}</p><button onclick="slaveCmd(${r.relay},'on')">Run</button><button onclick="slaveCmd(${r.relay},'off')">Stop</button></div>`).join('')
}
refreshMesh(true);setInterval(()=>refreshMesh(false),2000)
)JS";
  page.replace(originalRefresh, replacement);
  return page;
}

static void reliableHandleAdmin() {
  server.send(200, "text/html", reliableBuildAdminPage());
}

static void reliableClearNamespace(const char* name) {
  Preferences p;
  if (p.begin(name, false)) {
    p.clear();
    p.end();
  }
}

static void reliableHandleFactoryReset() {
  if (meshIsSlave()) reliableStopAllSlaveRelays("factory_reset");
  else allOff();
  reliableClearNamespace(RELIABLE_MASTER_PREF_NAMESPACE);
  reliableClearNamespace(RELIABLE_SLAVE_PREF_NAMESPACE);
  reliableClearNamespace(RELIABLE_RUN_PREF_NAMESPACE);
  reliableClearNamespace(RELIABLE_OBS_PREF_NAMESPACE);
  meshHandleFactoryReset();
}

static void reliableMigrateLegacyMasterSchedules() {
  if (!meshIsMaster() || reliableMasterScheduleCount > 0 ||
      reliableMasterScheduleRevision > 0 || meshScheduleCount == 0) {
    return;
  }
  for (uint8_t i = 0; i < meshScheduleCount &&
                      i < MESH_MAX_REMOTE_SCHEDULES; i++) {
    uint8_t slot = 0;
    uint8_t relay = 0;
    if (!meshDecodeLogicalChannel(meshSchedules[i].logicalChannel, slot, relay)) continue;
    ReliableSchedule& schedule =
        reliableMasterSchedules[reliableMasterScheduleCount++];
    schedule.enabled = meshSchedules[i].enabled;
    schedule.logicalChannel = meshSchedules[i].logicalChannel;
    schedule.localRelay = relay + 1;
    schedule.startHour = meshSchedules[i].startHour;
    schedule.startMinute = meshSchedules[i].startMinute;
    schedule.runMinutes = meshSchedules[i].runMinutes;
    schedule.daysMask = meshSchedules[i].daysMask;
    schedule.lastRunDateKey = -1;
    schedule.lastRunMinuteOfDay = -1;
  }
  reliableMasterScheduleRevision = reliableMasterScheduleCount ? 1 : 0;
  reliableSaveMasterSchedules();
}

static void reliableTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (meshIsSlave()) {
      remoteEnabled = false;

      // Suppress the original heartbeat sender. This task sends the richer
      // heartbeat that includes schedule revision and durable run events.
      meshLastHeartbeatMs = millis();

      if (millis() > 1000UL) meshNeutralizeLegacyControllerForSlave();
      meshServiceSlaveTimers();
      reliableObserveRelayTransitions();
      reliableServiceSlaveSchedules();

      if (WiFi.status() == WL_CONNECTED &&
          millis() - reliableLastHeartbeatMs >= MESH_HEARTBEAT_INTERVAL_MS) {
        reliableLastHeartbeatMs = millis();
        reliableSendHeartbeat();
      }
    } else {
      // The master stores and distributes schedules but never executes slave
      // schedule occurrences or presents a run without slave confirmation.
      meshScheduleCount = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void meshReliablePreInit() {
  // Register authoritative routes before the original mesh layer registers
  // compatibility handlers. WebServer resolves duplicate routes in insertion
  // order.
  server.on("/admin", HTTP_GET, reliableHandleAdmin);
  server.on("/api/mesh/state", HTTP_GET, reliableHandleState);
  server.on("/api/mesh/schedules", HTTP_GET, reliableHandleSchedulesGet);
  server.on("/api/mesh/schedules", HTTP_POST, reliableHandleSchedulesPost);
  server.on("/api/mesh/relay", HTTP_POST, reliableHandleMasterRelayControl);
  server.on("/api/slaves/register", HTTP_POST, reliableHandleSlaveHeartbeat);
  server.on("/api/slaves/heartbeat", HTTP_POST, reliableHandleSlaveHeartbeat);
  server.on("/api/slave/relay", HTTP_POST, reliableHandleSlaveRelayCommand);
  server.on("/api/slave/alloff", HTTP_POST, reliableHandleSlaveAllOff);
  server.on("/api/mesh/slave-local", HTTP_POST, reliableHandleSlaveLocalControl);
  server.on("/api/manual-run", HTTP_GET, reliableHandleRoleAwareManualRun);
  server.on("/api/relay", HTTP_GET, reliableHandleRoleAwareRelay);
  server.on("/api/state", HTTP_GET, reliableHandleRoleAwareState);
  server.on("/status", HTTP_GET, reliableHandleRoleAwareState);
  server.on("/api/factory-reset", HTTP_GET, reliableHandleFactoryReset);
}

void meshReliablePostInit() {
  reliableLoadMasterSchedules();
  reliableLoadSlaveSchedules();
  reliableLoadRunLog();
  reliableLoadObservations();

  reliableMigrateLegacyMasterSchedules();

  // Disable the previous master-side schedule dispatcher. Reliable schedules
  // remain in their own versioned store and are executed only by slaves.
  meshScheduleCount = 0;

  if (meshIsSlave()) {
    reliableCloseInterruptedRunsAfterBoot();
  }

  xTaskCreatePinnedToCore(
      reliableTask,
      "reliableMesh",
      16384,
      nullptr,
      1,
      &reliableTaskHandle,
      0);
}
