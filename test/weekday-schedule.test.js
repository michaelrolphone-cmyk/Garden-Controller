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
    expect(wrapper).toContain('mask = ALL_WEEKDAYS_MASK;');
  });

  test('gates only automatic schedule starts by local weekday', () => {
    expect(wrapper).toContain('checkScheduleWithWeekdays()');
    expect(wrapper).toContain('scheduleRunsToday(i, t.tm_wday)');
    expect(wrapper).toContain('startRun(schedule.zoneIndex, schedule.runMinutes, false);');
    expect(wrapper).toContain('Manual zone and spigot runs are not affected.');
  });

  test('provides a mobile weekday editor and JSON API', () => {
    expect(wrapper).toContain('server.on("/schedule-days", HTTP_GET, handleScheduleDaysPage);');
    expect(wrapper).toContain('server.on("/schedule-days/save", HTTP_POST, handleScheduleDaysSave);');
    expect(wrapper).toContain('server.on("/api/schedule-days", HTTP_GET, handleScheduleDaysApiGet);');
    expect(wrapper).toContain('server.on("/api/schedule-days", HTTP_POST, handleScheduleDaysApiPost);');
    expect(wrapper).toContain('Save Watering Days');
  });

  test('keeps the existing loop responsibilities while replacing only the scheduler call', () => {
    expect(wrapper).toContain('dns.processNextRequest();');
    expect(wrapper).toContain('server.handleClient();');
    expect(wrapper).toContain('updateWeatherFromOpenMeteo();');
    expect(wrapper).toContain('updateRunState();');
    expect(wrapper).toContain('updateZoneRgbLed();');
    expect(wrapper).toContain('connectSta(false);');
  });
});
