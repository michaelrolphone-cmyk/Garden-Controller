const fs = require('fs');
const path = require('path');

describe('e-ink zone display firmware', () => {
  const ino = fs.readFileSync(path.join(__dirname, '..', 'mcu', 'relay', 'GardenEInkZoneDisplay.ino'), 'utf8');

  test('uses configurable WiFi/API settings and local admin api', () => {
    expect(ino).toContain('char staSsid[32] = "";');
    expect(ino).toContain('char apiToken[96] = "";');
    expect(ino).toContain('void handleApiConfigGet()');
    expect(ino).toContain('void handleApiConfigSet()');
    expect(ino).toContain('server.on("/api/config", HTTP_GET, handleApiConfigGet);');
    expect(ino).toContain('server.on("/api/config", HTTP_POST, handleApiConfigSet);');
    expect(ino).toContain('Preferences prefs;');
    expect(ino).toContain('saveConfig();');
  });

  test('documents and uses explicit e-paper pinout constants', () => {
    expect(ino).toContain('static const uint8_t EPD_CS_PIN = 5;');
    expect(ino).toContain('static const uint8_t EPD_DC_PIN = 17;');
    expect(ino).toContain('static const uint8_t EPD_RST_PIN = 16;');
    expect(ino).toContain('static const uint8_t EPD_BUSY_PIN = 4;');
    expect(ino).toContain('GxEPD2_750_T7(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)');
  });

  test('automatically chooses full vs partial refresh based on redraw scope', () => {
    expect(ino).toContain('bool isSubstantialRedrawNeeded(const ControllerState& prev, const ControllerState& next)');
    expect(ino).toContain('if (a.active != b.active) return true;');
    expect(ino).toContain('if (needsFullRefresh || isSubstantialRedrawNeeded(currentState, next))');
    expect(ino).toContain('renderFullMap(next);');
    expect(ino).toContain('renderPartialLegend(next);');
    expect(ino).not.toContain('/api/refresh/full');
    expect(ino).not.toContain('/api/refresh/partial');
  });

  test('holds static image when state has not changed', () => {
    expect(ino).toContain('bool changed = !statesEqual(currentState, next);');
    expect(ino).toContain('if (changed) {');
  });

  test('renders polygon zone map from relay api state and logs sd snapshots', () => {
    expect(ino).toContain('const ZonePolygon ZONE_POLYGONS[]');
    expect(ino).toContain('doc["deviceTelemetry"]["zoneRuns"]');
    expect(ino).toContain('void renderZoneMap(const ControllerState& state)');
    expect(ino).toContain('SD.open("/zone_state.log", FILE_APPEND)');
  });
});
