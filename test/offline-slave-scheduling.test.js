const fs = require('fs');
const path = require('path');

describe('offline-resilient slave scheduling', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const reliable = fs.readFileSync(
    path.join(relayDir, 'ZZReliableSlaveScheduling.ino'),
    'utf8'
  );
  const gate = fs.readFileSync(path.join(relayDir, 'HerokuSyncGate.ino'), 'utf8');
  const startup = fs.readFileSync(path.join(relayDir, 'ZZZZZPostSetupServices.ino'), 'utf8');
  const header = fs.readFileSync(path.join(relayDir, 'MasterSlaveSupport.h'), 'utf8');
  const intervals = fs.readFileSync(
    path.join(relayDir, 'ZZZZZZZScheduleIntervalReconciliation.ino'),
    'utf8'
  );

  test('registers reliable routes before setup and loads runtime state after setup', () => {
    expect(header).toContain('void meshReliablePreInit();');
    expect(header).toContain('void meshReliablePostInit();');
    expect(gate.indexOf('meshReliablePreInit();')).toBeLessThan(
      gate.indexOf('meshPreSetupInitNoTask();')
    );
    expect(gate).not.toContain('meshReliablePostInit();');
    expect(startup).toContain('meshReliablePostInit();');
  });

  test('stores versioned schedule snapshots on both master and slave', () => {
    expect(reliable).toContain('RELIABLE_MASTER_PREF_NAMESPACE');
    expect(reliable).toContain('RELIABLE_SLAVE_PREF_NAMESPACE');
    expect(reliable).toContain('reliableMasterScheduleRevision');
    expect(reliable).toContain('reliableSlaveScheduleRevision');
    expect(reliable).toContain('reliableApplySlaveScheduleSnapshot');
  });

  test('master distributes schedules but does not execute slave schedule occurrences', () => {
    expect(reliable).toContain('meshScheduleCount = 0;');
    expect(reliable).toContain('never executes slave');
    expect(reliable).not.toContain('meshServiceMasterSchedules();');
  });

  test('slave relays are reconciled against absolute schedule intervals', () => {
    expect(intervals).toContain('intervalReconcileSlave');
    expect(intervals).toContain('intervalRemainingForSchedule');
    expect(intervals).toContain('end - current');
    expect(intervals).toContain('reliableStopSlaveRelay(relay, "completed")');
    expect(intervals).toContain('meshSlaveRuns[relayIndex].durationMs = remainingMs');
  });

  test('journals actual slave starts and stops until acknowledged', () => {
    expect(reliable).toContain('RELIABLE_RUN_EVENT_START');
    expect(reliable).toContain('RELIABLE_RUN_EVENT_STOP');
    expect(reliable).toContain('reliableSaveRunLog');
    expect(reliable).toContain('runEvents');
    expect(reliable).toContain('ackRunSequence');
    expect(reliable).toContain('reliableAcknowledgeRunEvents');
  });

  test('heartbeat current state is authoritative and offline state is unknown', () => {
    expect(reliable).toContain('stateConfirmed');
    expect(reliable).toContain('lastReportedOn');
    expect(reliable).toContain('`UNKNOWN · last report');
    expect(reliable).toContain('reportedRemainingSeconds');
  });

  test('synchronizes slave time from the master before local interval execution', () => {
    expect(reliable).toContain('reliableSyncClock');
    expect(reliable).toContain('masterTime');
    expect(intervals).toContain('if (clockIsValid())');
  });

  test('factory reset clears all reliable schedule and run namespaces', () => {
    expect(reliable).toContain('RELIABLE_MASTER_PREF_NAMESPACE');
    expect(reliable).toContain('RELIABLE_SLAVE_PREF_NAMESPACE');
    expect(reliable).toContain('RELIABLE_RUN_PREF_NAMESPACE');
    expect(reliable).toContain('RELIABLE_OBS_PREF_NAMESPACE');
    expect(reliable).toContain('reliableHandleFactoryReset');
  });
});
