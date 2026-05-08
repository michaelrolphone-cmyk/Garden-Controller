const fs = require('fs');
const path = require('path');

describe('e-ink schedule/news/weather firmware requirements', () => {
  const ino = fs.readFileSync(path.join(__dirname, '..', 'mcu', 'relay', 'GardenEInkZoneDisplay.ino'), 'utf8');

  test('uses required 7.5" driver class and required pin mapping', () => {
    expect(ino).toContain('GxEPD2_750_GDEY075T7');
    expect(ino).toContain('static const uint8_t EPD_MOSI_PIN = 14;');
    expect(ino).toContain('static const uint8_t EPD_SCLK_PIN = 13;');
    expect(ino).toContain('static const uint8_t EPD_CS_PIN = 15;');
    expect(ino).toContain('static const uint8_t EPD_DC_PIN = 27;');
    expect(ino).toContain('static const uint8_t EPD_RST_PIN = 26;');
    expect(ino).toContain('static const uint8_t EPD_BUSY_PIN = 25;');
    expect(ino).toContain('static const uint8_t SD_CS_PIN = 5;');
    expect(ino).toContain('static const uint8_t SD_MISO_PIN = 12;');
  });

  test('uses required schedule screen geometry and right column panels', () => {
    expect(ino).toContain('drawMap(8, 48, 424, 424);');
    expect(ino).toContain('drawWeatherWidget(432, 48, 360, 160);');
    expect(ino).toContain('drawSchedulePanel(432, 207, 360, 133);');
    expect(ino).toContain('drawRuntimePanel(432, 339, 360, 133);');
    expect(ino).toContain('Castle Hills Garden Watering Schedule');
  });

  test('implements required controller endpoints and state fields', () => {
    expect(ino).toContain('server.on("/state", HTTP_GET, handleState);');
    expect(ino).toContain('server.on("/extra", HTTP_GET, handleExtra);');
    expect(ino).toContain('server.on("/stop", HTTP_GET, handleStop);');
    expect(ino).toContain('server.on("/sync", HTTP_GET, handleSync);');
    expect(ino).toContain('server.on("/redraw", HTTP_GET, handleRedraw);');
    expect(ino).toContain('server.on("/saveZone", HTTP_GET, handleSaveZone);');
    expect(ino).toContain('server.on("/saveLogic", HTTP_GET, handleSaveLogic);');
    expect(ino).toContain('server.on("/saveNews", HTTP_POST, handleSaveNews);');
    expect(ino).toContain('server.on("/history.csv", HTTP_GET, handleHistoryCsv);');
    expect(ino).toContain('server.on("/clearHistory", HTTP_GET, handleClearHistory);');
    expect(ino).toContain('doc["title"] = state.title;');
    expect(ino).toContain('doc["gardenNews"] = state.gardenNews;');
    expect(ino).toContain('doc["currentRunActive"] = state.run.active;');
    expect(ino).toContain('doc["displayMode"] = state.displayMode;');
  });

  test('supports news/graph/schedule rotation and watering suppression', () => {
    expect(ino).toContain('Rotation period: 4 minutes');
    expect(ino).toContain('if (state.run.active) { strlcpy(state.displayMode, MODE_SCHEDULE, sizeof(state.displayMode)); return; }');
    expect(ino).toContain('Castle Hills Garden News');
    expect(ino).toContain('Current + Weekly Weather');
    expect(ino).toContain('drawScreen();');
  });

  test('consumes relay weather and time fields and updates runtime meter with partial refresh', () => {
    expect(ino).toContain('fetchRelayJson("/weather", wdoc)');
    expect(ino).toContain('state.weather.precipitationChancePct');
    expect(ino).toContain('fetchRelayJson("/time", tdoc)');
    expect(ino).toContain('display.setPartialWindow(432, 339, 360, 133);');
    expect(ino).toContain('drawRuntimePanel(432, 339, 360, 133);');
  });
});
