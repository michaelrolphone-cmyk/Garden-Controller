const fs = require('fs');
const path = require('path');

describe('post-setup services and master clock recovery', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const gate = fs.readFileSync(path.join(relayDir, 'HerokuSyncGate.ino'), 'utf8');
  const startup = fs.readFileSync(path.join(relayDir, 'ZZZZZPostSetupServices.ino'), 'utf8');
  const masterClock = fs.readFileSync(path.join(relayDir, 'ZZZZZZMasterClockRecovery.ino'), 'utf8');

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
    expect(startup).toContain('slaveScheduleCatchupPostInit();');
    expect(startup).toContain('masterClockRecoveryPostInit();');
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

  test('clock recovery queues both local zones and spigots', () => {
    expect(masterClock).toContain('MASTER_CLOCK_CATCHUP_WINDOW_MINUTES = 60');
    expect(masterClock).toContain('MASTER_CATCHUP_ZONE');
    expect(masterClock).toContain('MASTER_CATCHUP_SPIGOT');
    expect(masterClock).toContain('dailySchedules[i]');
    expect(masterClock).toContain('spigotSchedules[i]');
    expect(masterClock).toContain('masterCatchupBuildPending');
  });

  test('catch-up execution is sequential and avoids future regular schedules', () => {
    expect(masterClock).toContain('masterClockAnyLocalRunActive()');
    expect(masterClock).toContain('masterClockExactScheduleDue');
    expect(masterClock).toContain('masterClockNextScheduledMinute');
    expect(masterClock).toContain('endMinute > 20 * 60');
    expect(masterClock).toContain('startRun(schedule.zoneIndex');
    expect(masterClock).toContain('startSpigotRun(schedule.runMinutes)');
  });

  test('a clock that becomes valid during setup still triggers reconciliation', () => {
    expect(startup).toContain('gardenClockWasValidBeforeSetup = clockIsValid();');
    expect(masterClock).toContain('!gardenClockWasValidBeforeSetup && clockIsValid()');
    expect(masterClock).toContain('currentEpoch > masterClockLastEpoch + 90UL');
  });
});
