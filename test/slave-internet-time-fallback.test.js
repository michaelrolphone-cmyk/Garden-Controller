const fs = require('fs');
const path = require('path');

describe('slave Internet time fallback', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const gate = fs.readFileSync(path.join(relayDir, 'HerokuSyncGate.ino'), 'utf8');
  const header = fs.readFileSync(path.join(relayDir, 'MasterSlaveSupport.h'), 'utf8');
  const fallback = fs.readFileSync(path.join(relayDir, 'ZZZSlaveInternetTimeFallback.ino'), 'utf8');
  const catchup = fs.readFileSync(path.join(relayDir, 'ZZZZSlaveScheduleCatchup.ino'), 'utf8');

  test('starts fallback services after reliable schedule persistence is loaded', () => {
    expect(header).toContain('void slaveInternetTimePreInit();');
    expect(header).toContain('void slaveInternetTimePostInit();');
    expect(header).toContain('void slaveScheduleCatchupPostInit();');
    expect(gate.indexOf('slaveInternetTimePreInit();')).toBeLessThan(gate.indexOf('meshReliablePreInit();'));
    expect(gate.indexOf('meshReliablePostInit();')).toBeLessThan(gate.indexOf('slaveInternetTimePostInit();'));
    expect(gate.indexOf('slaveInternetTimePostInit();')).toBeLessThan(gate.indexOf('slaveScheduleCatchupPostInit();'));
  });

  test('persists a separate Internet-capable WiFi profile and NTP configuration', () => {
    expect(fallback).toContain('SLAVE_TIME_PREF_NAMESPACE[] = "r6timefallback"');
    expect(fallback).toContain('p.putString("ssid", slaveTimeFallbackSsid)');
    expect(fallback).toContain('p.putString("ntp1", slaveTimeNtpServer1)');
    expect(fallback).toContain('p.putUInt("lastNtp", slaveTimeLastInternetSyncEpoch)');
    expect(fallback).toContain('Fallback Internet WiFi SSID');
    expect(fallback).toContain('fallback Internet WiFi must be different from the master AP');
  });

  test('requires a valid direct NTP response before setting the clock', () => {
    expect(fallback).toContain('WiFiUDP udp');
    expect(fallback).toContain('udp.beginPacket(address, 123)');
    expect(fallback).toContain('seconds1900 - 2208988800UL');
    expect(fallback).toContain('settimeofday(&tv, nullptr)');
    expect(fallback).toContain('Internet connected but both NTP servers failed');
  });

  test('prefers the master but changes networks when the clock requires fallback', () => {
    expect(fallback).toContain('SLAVE_TIME_MASTER_GRACE_MS');
    expect(fallback).toContain('slaveTimeConnectInternet();');
    expect(fallback).toContain('slaveTimeConnectMaster();');
    expect(fallback).toContain('reliableLastHeartbeatMs = millis();');
    expect(fallback).toContain('master unavailable; local schedules using retained clock');
  });

  test('starts a new six-hour window after each successful Internet sync', () => {
    expect(catchup).toContain('slaveCatchupSeenInternetSyncEpoch');
    expect(catchup).toContain('slaveTimeMasterLostSinceMs = millis();');
    expect(catchup).toContain('instead of repeatedly switching networks every retry cycle');
  });

  test('reports clock validity and source to the master promptly', () => {
    expect(fallback).toContain('server.on("/api/slaves/time-status"');
    expect(fallback).toContain('doc["clockValid"] = clockIsValid()');
    expect(fallback).toContain('doc["timeSource"] = slaveTimeSource');
    expect(fallback).toContain('clock["valid"] = observation.clockValid');
    expect(fallback).toContain('Clock status not yet reported');
    expect(catchup).toContain('slaveTimeLastStatusReportMs = millis() - SLAVE_TIME_STATUS_REPORT_MS');
  });

  test('surfaces invalid clock state instead of silently skipping schedules', () => {
    expect(fallback).toContain('CRITICAL: clock invalid and Internet time WiFi is not configured');
    expect(fallback).toContain('CRITICAL: master and Internet time are unavailable; clock invalid');
    expect(fallback).toContain("status.style.fontWeight=tf.clockValid?'normal':'bold'");
  });

  test('catches up a recently missed schedule after clock acquisition', () => {
    expect(catchup).toContain('SLAVE_CLOCK_CATCHUP_WINDOW_MINUTES = 60');
    expect(catchup).toContain('meshIsSlave() && meshLegacyNeutralized');
    expect(catchup).toContain('slaveCatchupAfterClockRecovery');
    expect(catchup).toContain('minuteOfDay + schedule.runMinutes > 20 * 60');
    expect(catchup).toContain('slaveCatchupAnyRelayActive()');
    expect(catchup).toContain('"catchup"');
    expect(catchup).toContain('reliableSaveSlaveSchedules();');
  });

  test('factory reset clears fallback credentials', () => {
    expect(fallback).toContain('meshClearPreferencesNamespace(SLAVE_TIME_PREF_NAMESPACE)');
    expect(fallback).toContain('reliableHandleFactoryReset();');
  });
});
