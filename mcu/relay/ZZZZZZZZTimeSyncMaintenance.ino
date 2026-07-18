// Time-fallback maintenance that is independent of schedule execution.

static TaskHandle_t timeSyncMaintenanceTaskHandle = nullptr;
static uint32_t timeSyncMaintenanceSeenEpoch = 0;

static void timeSyncMaintenanceTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (slaveTimeLastInternetSyncEpoch > 0 &&
        slaveTimeLastInternetSyncEpoch != timeSyncMaintenanceSeenEpoch) {
      timeSyncMaintenanceSeenEpoch = slaveTimeLastInternetSyncEpoch;
      if (meshIsSlave()) {
        // Begin a fresh resynchronization interval after a confirmed NTP reply.
        slaveTimeMasterLostSinceMs = millis();
        // Report the new clock source to the master on the next service pass.
        slaveTimeLastStatusReportMs =
            millis() - SLAVE_TIME_STATUS_REPORT_MS;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void timeSyncMaintenancePostInit() {
  if (timeSyncMaintenanceTaskHandle != nullptr) return;
  timeSyncMaintenanceSeenEpoch = slaveTimeLastInternetSyncEpoch;
  BaseType_t created = xTaskCreatePinnedToCore(
      timeSyncMaintenanceTask,
      "timeMaintenance",
      3072,
      nullptr,
      1,
      &timeSyncMaintenanceTaskHandle,
      0);
  if (created != pdPASS) {
    timeSyncMaintenanceTaskHandle = nullptr;
    slaveTimeStatus = "CRITICAL: time fallback maintenance failed to start";
  }
}
