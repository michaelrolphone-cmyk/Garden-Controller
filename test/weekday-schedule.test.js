const fs = require('fs');
const path = require('path');

describe('relay firmware zone, weekday, and spigot schedule support', () => {
  const root = path.join(__dirname, '..');
  const relayDir = path.join(root, 'mcu', 'relay');
  const wrapper = fs.readFileSync(path.join(relayDir, 'GardenSimpleRelay6.ino'), 'utf8');
  const core = fs.readFileSync(path.join(relayDir, 'GardenSimpleRelay6Core.inc'), 'utf8');
  const start = fs.readFileSync(path.join(root, 'src', 'start.js'), 'utf8');

  test('preserves the existing relay firmware as the included core', () => {
    expect(wrapper).toContain('#include "GardenSimpleRelay6Core.inc"');
    expect(wrapper).toContain('#define setup gardenLegacySetup');
    expect(wrapper).toContain('#define loop gardenLegacyLoop');
    expect(wrapper).toContain('#define handleAdmin gardenLegacyHandleAdmin');
    expect(wrapper).toContain('#define setupServer gardenLegacySetupServer');
    expect(core).toContain('const char FIRMWARE_VERSION[] = "v26-stop-zone-api";');
  });

  test('keeps independent weekday masks for zone schedules', () => {
    expect(wrapper).toContain('static const uint8_t ALL_WEEKDAYS_MASK = 0x7F;');
    expect(wrapper).toContain('scheduleDaysMasks[MAX_DAILY_SCHEDULES]');
    expect(wrapper).toContain('zoneScheduleRunsToday(i, currentTime.tm_wday)');
    expect(wrapper).toContain('dayPrefs.putUChar((keyBase + "mask").c_str()');
  });

  test('stores persistent channel-6 schedules separately from zone schedules', () => {
    expect(wrapper).toContain('struct SpigotSchedule');
    expect(wrapper).toContain('spigotSchedules[MAX_DAILY_SCHEDULES]');
    expect(wrapper).toContain('spigotPrefs.begin("relay6spig", false)');
    expect(wrapper).toContain('spigotPrefs.getUChar("count", 0)');
    expect(wrapper).toContain('spigotSchedulesDirty');
  });

  test('starts a timed spigot run when a channel-6 schedule is due', () => {
    expect(wrapper).toContain('for (uint8_t i = 0; i < spigotScheduleCount; i++)');
    expect(wrapper).toContain('(schedule.daysMask & (1U << currentTime.tm_wday)) == 0');
    expect(wrapper).toContain('startSpigotRun(schedule.runMinutes);');
    expect(wrapper).toContain('updateRunState();');
  });

  test('integrates Spigots into the existing captive portal schedule manager', () => {
    expect(wrapper).toContain("[6,'Spigots']");
    expect(wrapper).toContain("fetch('/api/schedules-with-days'");
    expect(wrapper).toContain("alert('Zone and spigot schedules saved')");
    expect(wrapper).toContain('Channel 6 runs only the spigots');
    expect(wrapper).toContain('id=\\"schedule-manager\\"');
  });

  test('publishes and synchronizes spigot schedules with Heroku', () => {
    expect(wrapper).toContain('publishAllSchedulesNow()');
    expect(wrapper).toContain('doc["includesSpigotSchedules"] = true;');
    expect(wrapper).toContain('"/api/microcontroller/schedules"');
    expect(wrapper).toContain('"/api/firmware/spigot-schedules"');
    expect(wrapper).toContain('spigotScheduleSyncTask');
  });

  test('protects local schedules from restart and concurrent synchronization loss', () => {
    expect(wrapper).toContain('xSemaphoreCreateRecursiveMutex()');
    expect(wrapper).toContain('spigotScheduleRevision == publishedRevision');
    expect(wrapper).toContain('server schedule state empty; republishing local schedules');
    expect(start).toContain('spigotSchedulesAuthoritative');
    expect(start).toContain('authoritative: spigotSchedulesAuthoritative');
  });

  test('Heroku accepts channel 6 while preserving zone schedule commands', () => {
    expect(start).toContain('channel > MASTER_VALVE_CHANNEL');
    expect(start).toContain("findRoute(app, '/api/schedules', 'post')");
    expect(start).toContain("findRoute(app, '/gui/schedules', 'post')");
    expect(start).toContain("findRoute(app, '/gui/schedules/:id/delete', 'post')");
    expect(start).toContain("app.get('/api/firmware/spigot-schedules'");
    expect(start).toContain('preservedSpigots');
  });

  test('retains the four-to-eight schedule window and no-overlap validation', () => {
    expect(start).toContain('RUN_WINDOW_START_MINUTES = 4 * 60');
    expect(start).toContain('RUN_WINDOW_END_MINUTES = 20 * 60');
    expect(start).toContain('current.startMinutes < previous.endMinutes');
  });
});
