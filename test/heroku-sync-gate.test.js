const fs = require('fs');
const path = require('path');

describe('firmware Heroku synchronization gate', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const gate = fs.readFileSync(path.join(relayDir, 'HerokuSyncGate.ino'), 'utf8');
  const wrapper = fs.readFileSync(path.join(relayDir, 'GardenSimpleRelay6.ino'), 'utf8');
  const core = fs.readFileSync(path.join(relayDir, 'GardenSimpleRelay6Core.inc'), 'utf8');

  test('applies the gate before firmware setup starts', () => {
    expect(gate).toContain('void initVariant()');
    expect(gate).toContain('remoteEnabled = false;');
    expect(gate).toContain('settings.putBool(SYNC_ENABLED_KEY, false);');
  });

  test('persists initialization so the administrator setting is honored later', () => {
    expect(gate).toContain('SYNC_INITIALIZED_KEY[] = "syncManaged"');
    expect(gate).toContain('settings.getBool(SYNC_INITIALIZED_KEY, false)');
    expect(gate).toContain('remoteEnabled = settings.getBool(SYNC_ENABLED_KEY, false);');
    expect(core).toContain('prefs.putBool("remoteEn", remoteEnabled);');
  });

  test('uses the existing captive-portal admin switch', () => {
    expect(core).toContain('<input type="checkbox" id="remoteEnabled">');
    expect(core).toContain('remoteEnabled:document.getElementById(\'remoteEnabled\').checked');
    expect(core).toContain('document.getElementById(\'remoteEnabled\').checked=!!s.remoteEnabled');
  });

  test('blocks every Heroku synchronization lane while disabled', () => {
    expect(core).toContain('return remoteEnabled && strlen(remoteApiBase) > 0');
    expect(core).toContain('void publishFullStateNow()');
    expect(core).toContain('if (!remoteReady()) return;');
    expect(wrapper).toContain('if (remoteReady())');
    expect(wrapper).toContain('syncSpigotSchedulesFromRemote();');
  });
});
