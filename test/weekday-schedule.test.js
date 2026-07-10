const fs = require('fs');
const path = require('path');

describe('relay firmware weekday schedule support', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const wrapper = fs.readFileSync(path.join(relayDir, 'GardenSimpleRelay6.ino'), 'utf8');
  const core = fs.readFileSync(path.join(relayDir, 'GardenSimpleRelay6Core.inc'), 'utf8');

  test('preserves the existing relay firmware as the included core', () => {
    expect(wrapper).toContain('#include "GardenSimpleRelay6Core.inc"');
    expect(wrapper).toContain('#define setup gardenLegacySetup');
    expect(wrapper).toContain('#define loop gardenLegacyLoop');
    expect(wrapper).toContain('#define handleAdmin gardenLegacyHandleAdmin');
    expect(wrapper).toContain('#define setupServer gardenLegacySetupServer');
    expect(core).toContain('const char FIRMWARE_VERSION[] = "v26-stop-zone-api";');
    expect(core).toContain('void checkSchedule()');
  });

  test('stores a seven-bit mask for every schedule', () => {
    expect(wrapper).toContain('static const uint8_t ALL_WEEKDAYS_MASK = 0x7F;');
    expect(wrapper).toContain('scheduleDaysMasks[MAX_DAILY_SCHEDULES]');
    expect(wrapper).toContain('dayPrefs.putUChar((keyBase + "mask").c_str()');
    expect(wrapper).toContain('dayPrefs.getUChar((keyBase + "mask").c_str(), ALL_WEEKDAYS_MASK)');
  });

  test('matches persisted masks to schedule signatures after edits or reordering', () => {
    expect(wrapper).toContain('uint32_t scheduleDaysSignature(const DailySchedule& schedule)');
    expect(wrapper).toContain('oldSignatures[oldIndex] == signature');
    expect(wrapper).toContain('uint8_t mask = ALL_WEEKDAYS_MASK;');
  });

  test('gates only automatic schedule starts by local weekday', () => {
    expect(wrapper).toContain('void checkScheduleWithWeekdays()');
    expect(wrapper).toContain('scheduleRunsToday(i, t.tm_wday)');
    expect(wrapper).toContain('startRun(schedule.zoneIndex, schedule.runMinutes, false);');
    expect(wrapper).toContain('checkScheduleWithWeekdays();');
  });

  test('integrates weekday controls into the captive portal admin schedule manager', () => {
    expect(wrapper).toContain('String buildIntegratedAdminPage()');
    expect(wrapper).toContain('id=\\"schedule-manager\\"');
    expect(wrapper).toContain('class="weekday-picker"');
    expect(wrapper).toContain("const WEEKDAY_NAMES=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];");
    expect(wrapper).toContain("document.querySelectorAll('#adminSchedRows .schedule-editor-row')");
    expect(wrapper).toContain("fetch('/api/schedule-days',{");
    expect(wrapper).toContain("alert('Schedules and watering days saved')");
  });

  test('uses the existing Save Schedules action for timing and weekday masks', () => {
    expect(wrapper).toContain("const timingResponse=await fetch('/api/schedules'");
    expect(wrapper).toContain("const daysResponse=await fetch('/api/schedule-days'");
    expect(wrapper).toContain('daysMask:weekdayMaskForRow(row)');
    expect(wrapper).toContain('schedule-enabled');
  });

  test('retires the separate weekday editor in favor of the admin anchor', () => {
    expect(wrapper).toContain('void handleScheduleDaysRedirect()');
    expect(wrapper).toContain('server.sendHeader("Location", "/admin#schedule-manager", true);');
    expect(wrapper).toContain('server.on("/schedule-days", HTTP_GET, handleScheduleDaysRedirect);');
    expect(wrapper).not.toContain('String scheduleDaysPage()');
  });

  test('restores the complete existing route set and controller initialization', () => {
    expect(wrapper).toContain('server.on("/admin", HTTP_GET, handleAdmin);');
    expect(wrapper).toContain('server.on("/api/manual-run", HTTP_GET, handleManualRun);');
    expect(wrapper).toContain('server.on("/api/remote/test", HTTP_GET, handleRemoteTest);');
    expect(wrapper).toContain('xTaskCreatePinnedToCore(');
    expect(wrapper).toContain('updateWeatherFromOpenMeteo();');
    expect(wrapper).toContain('updateRunState();');
    expect(wrapper).toContain('updateZoneRgbLed();');
    expect(wrapper).toContain('connectSta(false);');
  });
});
