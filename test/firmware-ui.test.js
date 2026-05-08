const fs = require('fs');
const path = require('path');

describe('firmware local admin/mobile api integration', () => {
  const ino = fs.readFileSync(path.join(__dirname, '..', 'mcu', 'relay', 'GardenSimpleRelay6.ino'), 'utf8');

  test('exposes config/features/schedules api routes', () => {
    expect(ino).toContain('server.on("/api/features", HTTP_GET, handleApiFeatures);');
    expect(ino).toContain('server.on("/api/config", HTTP_GET, handleApiConfigGet);');
    expect(ino).toContain('server.on("/api/config", HTTP_POST, handleApiConfigSet);');
    expect(ino).toContain('server.on("/api/schedules", HTTP_POST, handleApiScheduleSet);');
  });

  test('admin ui includes direct relay on/off controls and api schedule manager', () => {
    expect(ino).toContain("/api/relay?zone=");
    expect(ino).toContain('Turn On');
    expect(ino).toContain('Turn Off');
    expect(ino).toContain('Castle Hills Garden Manager (Firmware Local)');
    expect(ino).toContain('Garden Zone Map');
    expect(ino).toContain('Schedule Timeline');
    expect(ino).toContain("setInterval(()=>refresh(false),1000)");
  });

  test('admin ui maps zone activity, color-codes map zones, and provides schedule CRUD inputs', () => {
    expect(ino).toContain('zone-color-1');
    expect(ino).toContain('zone-color-2');
    expect(ino).toContain('zone-color-3');
    expect(ino).toContain('zone-color-4');
    expect(ino).toContain('zone-color-5');
    expect(ino).toContain('zoneChannel(z)');
    expect(ino).toContain("d.zoneName||('Zone '+d.zone)");
    expect(ino).toContain('addScheduleRow(');
    expect(ino).toContain('saveSchedules()');
    expect(ino).toContain("#adminSchedRows .actions");
    expect(ino).toContain('lastScheduleKey');
    expect(ino).toContain('scheduleEditorBusy()');
    expect(ino).toContain('const shouldRedrawSchedules=forceScheduleRedraw||nextScheduleKey!==lastScheduleKey');
    expect(ino).toContain('class=\"relay-card zone-color-${zoneChannel(z)}\"');
  });

  test('firmware admin restores critical device settings and maintenance controls', () => {
    expect(ino).toContain('AP SSID');
    expect(ino).toContain('AP Password');
    expect(ino).toContain('Home WiFi SSID');
    expect(ino).toContain('Home WiFi Password');
    expect(ino).toContain('Remote API Base URL');
    expect(ino).toContain('Device ID / local metadata');
    expect(ino).toContain('API Token');
    expect(ino).toContain('Garden latitude');
    expect(ino).toContain('Garden longitude');
    expect(ino).toContain('Garden IANA timezone');
    expect(ino).toContain('ESP32 POSIX timezone');
    expect(ino).toContain('Test Remote API');
    expect(ino).toContain('Turn Off All Relays');
    expect(ino).toContain('Factory Reset');
    expect(ino).toContain("fetch('/api/config'");
    expect(ino).toContain("fetch('/api/remote/test')");
    expect(ino).toContain("fetch('/api/alloff')");
    expect(ino).toContain("fetch('/api/factory-reset')");
  });
  test('admin ui separates remote api test section and places wifi settings below schedules with bottom factory reset', () => {
    const wifiIdx = ino.indexOf('<h2>WiFi Settings</h2>');
    const schedulesIdx = ino.indexOf('<h2>Zones (scheduled irrigation)</h2>');
    const remoteSettingsIdx = ino.indexOf('<h2>Remote API Settings</h2>');
    const factoryResetIdx = ino.indexOf('<h2>Factory Reset</h2>');

    expect(schedulesIdx).toBeGreaterThan(-1);
    expect(wifiIdx).toBeGreaterThan(schedulesIdx);
    expect(factoryResetIdx).toBeGreaterThan(remoteSettingsIdx);
    expect(ino).toContain('class="actions header-actions"');
    expect(ino).not.toContain(">Test Buzzer<");
    expect(ino).toContain('flex-wrap:wrap');
    expect(ino).toContain("function hydrateConfig(s){document.getElementById('apSsid').value=s.apSsid||''");
    expect(ino).toContain("document.getElementById('remoteApiBase').value=s.remoteApiBase||''");
    expect(ino).toContain("document.getElementById('remoteDeviceId').value=s.remoteDeviceId||''");
    expect(ino).toContain("document.getElementById('remoteApiKey').value=s.remoteApiKey||''");
    expect(ino).toContain('doc["staSsid"] = staSsid;');
    expect(ino).toContain('doc["remoteApiBase"] = remoteApiBase;');
  });

});
