// One synchronous reconciliation pass used during post-setup startup.

void scheduleIntervalReconcileNow() {
  if (!clockIsValid()) return;

  struct tm now;
  if (!getLocalTime(&now, 20)) return;

  if (meshIsSlave()) {
    if (!meshLegacyNeutralized) {
      meshNeutralizeLegacyControllerForSlave();
    }
    intervalReconcileSlave(now);
  } else {
    intervalReconcileMaster(now);
  }
}
