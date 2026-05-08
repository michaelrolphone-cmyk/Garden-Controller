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
});
