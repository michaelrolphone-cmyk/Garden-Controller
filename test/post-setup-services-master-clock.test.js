const fs = require('fs');
const path = require('path');

describe('post-setup services and master clock recovery', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const gate = fs.readFileSync(path.join(relayDir, 'HerokuSyncGate.ino'), 'utf8');
  const startup = fs.readFileSync(path.join(relayDir, 'ZZZZZPostSetupServices.ino'), 'utf8');
  const masterClock = fs.readFileSync(path.join(relayDir, 'ZZZZZZMasterClockRecovery.ino'), 'utf8');
  const intervals = fs.readFileSync(path.join(relayDir, 'ZZZZZZZScheduleIntervalReconciliation.ino'), 'utf8');

  test('initVariant does not start hardware-touching services', () => {
    expect(gate).toContain('meshPreSetupInitNoTask();');
    expect(gate).toContain('gardenArmPostSetupServices();');
    expect(gate).not.toContain('meshEarlyInit();');
    expect(gate).not.toContain('meshReliablePostInit();');
    expect(gate).not.toContain('slaveInternetTimePostInit();');
    expect(gate).not.toContain('slaveScheduleCatchupPostInit();');
  });

  test('pre-setup mesh initialization registers routes without starting meshTask', () => {
    const preStart = startup.indexOf('void meshPreSetupInitNoTask()');
    const preEnd = startup.indexOf('static bool gardenCoreSetupReady()');
    const preSetupBody = startup.slice(preStart, preEnd);
    expect(preSetupBody).toContain('meshLoadPreferences();');
    expect(preSetupBody).toContain('meshSetSlaveApSubnet();');
    expect(preSetupBody).toContain('server.on("/api/slaves/heartbeat"');
    expect(preSetupBody).not.toContain('xTaskCreatePinnedToCore');
    expect(preSetupBody).not.toContain('meshTask,');
  });

  test('services wait for the normal setup completion marker', () => {
    expect(startup).toContain('__atomic_load_n(&remoteTaskHandle');
    expect(startup).toContain('spigotScheduleMutex == nullptr');
    expect(startup.indexOf('while (!gardenCoreSetupReady())')).toBeLessThan(
      startup.indexOf('gardenStartPostSetupServices();')
    );
    expect(startup).toContain('meshReliablePostInit();');
    expect(startup).toContain('slaveInternetTimePostInit();');
    expect(startup).not.toContain('slaveScheduleCatchupPostInit();');
    expect(startup).toContain('masterClockRecoveryPostInit();');
    expect(startup).toContain('scheduleIntervalReconciliationPostInit();');
  });

  test('slave cloud access remains disabled throughout setup', () => {
    expect(startup).toContain('if (meshIsSlave()) remoteEnabled = false;');
    expect(startup).toContain('slave services started after safe hardware initialization');
  });

  test('master actively obtains verified time and can use fallback WiFi', () => {
    expect(masterClock).toContain('slaveTimeQueryNtp(slaveTimeNtpServer1');
    expect(masterClock).toContain('slaveTimeQueryNtp(slaveTimeNtpServer2');
    expect(masterClock).toContain('masterClockConnectFallback();');
    expect(masterClock).toContain('masterClockReturnToPrimary();');
    expect(masterClock).toContain('master-internet-ntp');
    expect(masterClock).toContain('CRITICAL: master clock invalid');
  });

  test('clock recovery no longer creates delayed full-duration catch-up runs', () => {
    expect(masterClock).not.toContain('MASTER_CLOCK_CATCHUP_WINDOW_MINUTES');
    expect(masterClock).not.toContain('MasterCatchupOccurrence');
    expect(masterClock).not.toContain('startRun(');
    expect(masterClock).not.toContain('startSpigotRun(');
  });

  test('relay recovery is based on current schedule intervals', () => {
    expect(intervals).toContain('intervalRemainingForSchedule');
    expect(intervals).toContain('end - current');
    expect(intervals).toContain('intervalReconcileMaster');
    expect(intervals).toContain('intervalReconcileSlave');
    expect(intervals).toContain('remainingSeconds');
    expect(intervals).not.toContain('CATCHUP_WINDOW');
  });
});
