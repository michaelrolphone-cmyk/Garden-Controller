// Master clock recovery.
//
// Scheduled relay state is reconciled separately from absolute start/end
// intervals. This service is responsible only for obtaining and maintaining a
// valid clock for the master.

static const uint32_t MASTER_CLOCK_INITIAL_GRACE_MS = 15000UL;
static const uint32_t MASTER_CLOCK_RETRY_MS = 60000UL;
static const uint32_t MASTER_CLOCK_CONNECT_TIMEOUT_MS = 30000UL;

enum MasterClockNetworkPhase : uint8_t {
  MASTER_CLOCK_PRIMARY_NETWORK = 0,
  MASTER_CLOCK_CONNECTING_FALLBACK = 1,
  MASTER_CLOCK_RETURNING_PRIMARY = 2
};

static TaskHandle_t masterClockRecoveryTaskHandle = nullptr;
static MasterClockNetworkPhase masterClockNetworkPhase =
    MASTER_CLOCK_PRIMARY_NETWORK;
static uint32_t masterClockServiceStartedMs = 0;
static uint32_t masterClockPhaseStartedMs = 0;
static uint32_t masterClockLastAttemptMs = 0;
static bool masterClockFallbackNtpAttempted = false;

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
          millis() - masterClockPhaseStartedMs >=
              MASTER_CLOCK_CONNECT_TIMEOUT_MS) {
        masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
      }
      return;
    }

    if (slaveTimeSource == "unsynchronized" ||
        slaveTimeSource == "local") {
      slaveTimeSource = "master-system-sntp";
      slaveTimeStatus = "master clock valid through system NTP";
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
          slaveTimeStatus =
              "CRITICAL: fallback WiFi connected but both NTP servers failed";
        }
        masterClockReturnToPrimary();
      }
    } else if (millis() - masterClockPhaseStartedMs >=
               MASTER_CLOCK_CONNECT_TIMEOUT_MS) {
      slaveTimeStatus =
          "CRITICAL: fallback Internet time WiFi connection failed";
      masterClockReturnToPrimary();
    }
    return;
  }

  if (masterClockNetworkPhase == MASTER_CLOCK_RETURNING_PRIMARY) {
    if (slaveTimeConnectedTo(staSsid) ||
        millis() - masterClockPhaseStartedMs >=
            MASTER_CLOCK_CONNECT_TIMEOUT_MS) {
      masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
    }
    return;
  }

  if (millis() - masterClockServiceStartedMs <
      MASTER_CLOCK_INITIAL_GRACE_MS) {
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
  if (WiFi.status() == WL_CONNECTED && masterClockQueryCurrentNetwork()) {
    return;
  }

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
  if (!meshIsMaster() || masterClockRecoveryTaskHandle != nullptr) return;

  masterClockServiceStartedMs = millis();
  masterClockPhaseStartedMs = millis();
  masterClockLastAttemptMs = 0;
  masterClockNetworkPhase = MASTER_CLOCK_PRIMARY_NETWORK;
  masterClockFallbackNtpAttempted = false;

  BaseType_t created = xTaskCreatePinnedToCore(
      masterClockRecoveryTask,
      "masterClock",
      8192,
      nullptr,
      1,
      &masterClockRecoveryTaskHandle,
      0);
  if (created != pdPASS) {
    masterClockRecoveryTaskHandle = nullptr;
    slaveTimeStatus = "CRITICAL: master clock recovery task failed to start";
  }
}
