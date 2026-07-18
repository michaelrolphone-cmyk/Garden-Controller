// Post-setup service coordinator.
//
// initVariant() still has to load the persisted Master/Slave role and register
// authoritative routes before setupServer(). It must not start tasks that can
// touch GPIO, WiFi, schedules, or time before the normal sketch setup has made
// those facilities safe. This file separates those two lifecycle phases.

static TaskHandle_t gardenPostSetupGateTaskHandle = nullptr;
static volatile bool gardenPostSetupServicesStarted = false;
static bool gardenClockWasValidBeforeSetup = false;

void meshPreSetupInitNoTask() {
  gardenClockWasValidBeforeSetup = clockIsValid();

  meshMutex = xSemaphoreCreateRecursiveMutex();
  meshLoadPreferences();
  if (meshIsSlave()) {
    meshSetSlaveApSubnet();
    remoteEnabled = false;
  }

  // Registered before setupServer(); WebServer resolves duplicate routes in
  // insertion order. This phase performs no network I/O and starts no tasks.
  server.on("/", HTTP_GET, meshHandleRoleAwareRoot);
  server.on("/admin", HTTP_GET, meshHandleAdminShell);
  server.on("/admin/core", HTTP_GET, meshHandleAdminCore);
  server.on("/api/state", HTTP_GET, meshHandleRoleAwareState);
  server.on("/status", HTTP_GET, meshHandleRoleAwareState);
  server.on("/api/manual-run", HTTP_GET, meshHandleRoleAwareManualRun);
  server.on("/api/relay", HTTP_GET, meshHandleRoleAwareRelay);
  server.on("/api/spigots-run", HTTP_GET, meshHandleRoleAwareSpigots);
  server.on("/api/alloff", HTTP_GET, meshHandleRoleAwareAllOff);
  server.on("/api/factory-reset", HTTP_GET, meshHandleFactoryReset);

  server.on("/api/mesh/state", HTTP_GET, meshHandleState);
  server.on("/api/mesh/config", HTTP_POST, meshHandleConfigPost);
  server.on("/api/mesh/relay", HTTP_POST, meshHandleMasterRelayControl);
  server.on("/api/mesh/schedules", HTTP_GET, meshHandleSchedulesGet);
  server.on("/api/mesh/schedules", HTTP_POST, meshHandleSchedulesPost);
  server.on("/api/mesh/slave-local", HTTP_POST, meshHandleSlaveLocalControl);
  server.on("/api/slaves/register", HTTP_POST, meshHandleSlaveHeartbeat);
  server.on("/api/slaves/heartbeat", HTTP_POST, meshHandleSlaveHeartbeat);
  server.on("/api/slave/relay", HTTP_POST, meshHandleSlaveRelayCommand);
  server.on("/api/slave/alloff", HTTP_POST, meshHandleSlaveAllOff);
}

static bool gardenCoreSetupReady() {
  TaskHandle_t remoteHandle = __atomic_load_n(&remoteTaskHandle, __ATOMIC_ACQUIRE);
  if (remoteHandle != nullptr) return true;

  // The normal setup creates remoteTaskHandle after GPIO, configuration,
  // timezone, AP/station WiFi, NTP, weather initialization, and server.begin().
  // If that unrelated task allocation fails, do not permanently suppress the
  // watering services; use a conservative elapsed-time and initialized-state
  // fallback instead.
  if (millis() < 60000UL || spigotScheduleMutex == nullptr) return false;
  return (uint32_t)WiFi.softAPIP() != 0;
}

static bool gardenCreateTaskIfMissing(
    TaskHandle_t& handle,
    TaskFunction_t function,
    const char* name,
    uint32_t stackWords) {
  if (handle != nullptr) return true;
  BaseType_t created = xTaskCreatePinnedToCore(
      function,
      name,
      stackWords,
      nullptr,
      1,
      &handle,
      0);
  return created == pdPASS && handle != nullptr;
}

static void gardenStartPostSetupServices() {
  if (gardenPostSetupServicesStarted) return;

  // Load durable mesh/schedule state and start network services only after the
  // core controller has made GPIO, WiFi, schedules, and time safe to use.
  meshReliablePostInit();
  slaveInternetTimePostInit();

  // Retry a failed task allocation once with the already-loaded state. This
  // avoids a device appearing healthy while a critical scheduler task is absent.
  bool reliableReady = gardenCreateTaskIfMissing(
      reliableTaskHandle,
      reliableTask,
      "reliableMesh",
      16384);
  bool timeReady = gardenCreateTaskIfMissing(
      slaveTimeTaskHandle,
      slaveTimeTask,
      "slaveTime",
      8192);
  bool meshReady = gardenCreateTaskIfMissing(
      meshTaskHandle,
      meshTask,
      "meshTask",
      12288);

  masterClockRecoveryPostInit();
  scheduleIntervalReconciliationPostInit();

  gardenPostSetupServicesStarted = true;
  if (!(reliableReady && timeReady && meshReady)) {
    meshLastStatus = "CRITICAL: one or more post-setup services failed to start";
    slaveTimeStatus = "CRITICAL: watering service task allocation failed";
  } else if (meshIsSlave()) {
    meshLastStatus = "slave services started after safe hardware initialization";
  }
}

static void gardenPostSetupGateTask(void* parameter) {
  (void)parameter;
  while (!gardenCoreSetupReady()) {
    // loadConfig() runs during setup and may reload a previously enabled cloud
    // setting. Keep Slave mode locally isolated until the full slave runtime is
    // allowed to start after setup.
    if (meshIsSlave()) remoteEnabled = false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (meshIsSlave()) remoteEnabled = false;
  gardenStartPostSetupServices();
  gardenPostSetupGateTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void gardenArmPostSetupServices() {
  if (gardenPostSetupGateTaskHandle != nullptr || gardenPostSetupServicesStarted) return;

  BaseType_t created = xTaskCreatePinnedToCore(
      gardenPostSetupGateTask,
      "postSetupGate",
      4096,
      nullptr,
      1,
      &gardenPostSetupGateTaskHandle,
      0);
  if (created != pdPASS) {
    gardenPostSetupGateTaskHandle = nullptr;
    lastRemoteStatus = "CRITICAL: unable to create post-setup service gate";
  }
}
