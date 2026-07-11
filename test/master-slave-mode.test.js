const fs = require('fs');
const path = require('path');

describe('dual-role master/slave firmware support', () => {
  const relayDir = path.join(__dirname, '..', 'mcu', 'relay');
  const gate = fs.readFileSync(path.join(relayDir, 'HerokuSyncGate.ino'), 'utf8');
  const mesh = fs.readFileSync(path.join(relayDir, 'MasterSlaveSupport.ino'), 'utf8');
  const header = fs.readFileSync(path.join(relayDir, 'MasterSlaveSupport.h'), 'utf8');

  test('initializes mesh role before the legacy setup and route registration', () => {
    expect(header).toContain('void meshEarlyInit();');
    expect(gate).toContain('#include "MasterSlaveSupport.h"');
    expect(gate).toContain('meshEarlyInit();');
    expect(mesh).toContain('server.on("/admin", HTTP_GET, meshHandleAdminShell);');
    expect(mesh).toContain('server.on("/admin/core", HTTP_GET, meshHandleAdminCore);');
  });

  test('uses one firmware with persisted master and slave roles', () => {
    expect(mesh).toContain('MESH_ROLE_MASTER');
    expect(mesh).toContain('MESH_ROLE_SLAVE');
    expect(mesh).toContain('p.putUChar("role", (uint8_t)meshRole)');
    expect(mesh).toContain('Device role<select id="meshRole"');
    expect(mesh).toContain('Save Role Settings and Restart');
  });

  test('avoids AP subnet collision when a slave joins the master AP', () => {
    expect(mesh).toContain('apIp = IPAddress(192, 168, 5, 1);');
    expect(mesh).toContain('apGw = IPAddress(192, 168, 5, 1);');
    expect(mesh).toContain('meshMasterHost[48] = "192.168.4.1"');
  });

  test('reports slave presence and allocates six logical zones per slave', () => {
    expect(mesh).toContain('MESH_RELAYS_PER_SLAVE = 6');
    expect(mesh).toContain('MESH_FIRST_LOGICAL_CHANNEL = 7');
    expect(mesh).toContain('server.on("/api/slaves/heartbeat", HTTP_POST, meshHandleSlaveHeartbeat);');
    expect(mesh).toContain('baseLogicalChannel');
    expect(mesh).toContain('meshSlaveOnline');
  });

  test('turns all six slave relays into independent timed zone outputs', () => {
    expect(mesh).toContain('MeshSlaveRun meshSlaveRuns[MESH_RELAYS_PER_SLAVE]');
    expect(mesh).toContain('meshStartSlaveRelay');
    expect(mesh).toContain('setRelay(relayIndex, true);');
    expect(mesh).toContain('server.on("/api/slave/relay", HTTP_POST, meshHandleSlaveRelayCommand);');
    expect(mesh).toContain('relay 6 is a normal slave zone in slave mode');
  });

  test('keeps master channel 6 as spigots and gives slave zones channels 7 and above', () => {
    expect(mesh).toContain('1-5  local master zones');
    expect(mesh).toContain('6    local master spigots');
    expect(mesh).toContain('7-12 first slave relays 1-6');
  });

  test('supports master-owned schedules for remote zones', () => {
    expect(mesh).toContain('struct MeshRemoteSchedule');
    expect(mesh).toContain('meshServiceMasterSchedules');
    expect(mesh).toContain('server.on("/api/mesh/schedules", HTTP_POST, meshHandleSchedulesPost);');
    expect(mesh).toContain('enabled schedules must stay between 04:00 and 20:00');
    expect(mesh).toContain('daysMask');
  });

  test('prevents slave devices from synchronizing directly with Heroku', () => {
    expect(mesh).toContain('remoteEnabled = false;');
    expect(mesh).toContain('A slave never talks directly to Heroku');
  });

  test('factory reset clears mesh, weekday, and spigot state', () => {
    expect(mesh).toContain('meshClearPreferencesNamespace(MESH_PREF_NAMESPACE);');
    expect(mesh).toContain('meshClearPreferencesNamespace("relay6days");');
    expect(mesh).toContain('meshClearPreferencesNamespace("relay6spig");');
  });
});
