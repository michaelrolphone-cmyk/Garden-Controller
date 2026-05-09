const fs = require('fs');
const path = require('path');

describe('e-ink schedule/news/weather firmware requirements', () => {
  const ino = fs.readFileSync(path.join(__dirname, '..', 'mcu', 'relay', 'GardenEInkZoneDisplay.ino'), 'utf8');

  test('uses required 7.5" driver class and required pin mapping', () => {
    expect(ino).toContain('GxEPD2_750_GDEY075T7');
    expect(ino).toContain('static const uint8_t EPD_MOSI_PIN = 23;');
    expect(ino).toContain('static const uint8_t EPD_SCLK_PIN = 18;');
    expect(ino).toContain('static const uint8_t EPD_CS_PIN = 27;');
    expect(ino).toContain('static const uint8_t EPD_DC_PIN = 14;');
    expect(ino).toContain('static const uint8_t EPD_RST_PIN = 33;');
    expect(ino).toContain('static const uint8_t EPD_BUSY_PIN = 13;');
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
    expect(ino).toContain('if (cycleMs < 45000UL)');
    expect(ino).toContain('else if (cycleMs >= 120000UL && cycleMs < 165000UL)');
    expect(ino).toContain('Castle Hills Garden News');
    expect(ino).toContain('Current + Weekly Weather');
    expect(ino).toContain('drawScreen();');
  });



  test('draws polygon zone map with active-zone inversion behavior', () => {
    expect(ino).toContain('const Pt Z1[]');
    expect(ino).toContain('void fillPolyHatch(const Pt* p, int n, bool active)');
    expect(ino).toContain('drawZoneLabel(34,100,1,a1);');
    expect(ino).toContain('display.setTextColor(GxEPD_WHITE);');
  });

  test('panel layout uses single 1px border calls without decorative border styles in firmware drawing', () => {
    expect(ino).toContain('display.drawRect(x,y,w,h,GxEPD_BLACK);');
    expect(ino).not.toContain('drawRoundRect');
  });

  test('draws weather compass and sunrise/sunset strip in widget', () => {
    expect(ino).toContain('char clippedCondition[20]; snprintf(clippedCondition, sizeof(clippedCondition), "%.18s", state.weather.condition);');
    expect(ino).toContain('display.fillCircle(x+136,y+18,2,GxEPD_BLACK);');
    expect(ino).toContain('display.drawCircle(cx,cy,r,GxEPD_BLACK);');
    expect(ino).toContain('display.print("Sunrise")');
    expect(ino).toContain('display.print("Sunset")');
    expect(ino).toContain('display.fillTriangle(cx,cy');
    expect(ino).toContain('cos(rad+2.75f)*10');
    expect(ino).toContain('for (int hx = x+94; hx <= x+266; hx += 8) display.drawLine(hx, y+154, hx+4, y+154, GxEPD_BLACK);');
    expect(ino).toContain('sqrtf(max(0.0f, 1.0f - norm * norm))');
    expect(ino).toContain('formatTimeLowerNoLeadingZero(state.weather.sunriseEpoch, sunriseTxt, sizeof(sunriseTxt));');
    expect(ino).toContain('float windDeg = state.weather.windDeg == 0 && strcmp(state.weather.windDirection, "N") != 0 ? directionToDegrees(state.weather.windDirection) : (float)state.weather.windDeg;');
    expect(ino).toContain('display.printf("Precip. chance %.0f%%", state.weather.precipitationChancePct);');
    expect(ino).toContain('state.weather.windDeg');
  });

  test('applies visual typography rules for headers and body text', () => {
    expect(ino).toContain('#include <Fonts/FreeMonoBold12pt7b.h>');
    expect(ino).toContain('#include <Fonts/FreeSans9pt7b.h>');
    expect(ino).toContain('display.setFont(&FreeMonoBold12pt7b);');
    expect(ino).toContain('display.setFont(&FreeSans9pt7b);');
  });

  test('supports queue and ledger maintenance endpoints', () => {
    expect(ino).toContain('server.on("/queue/clear", HTTP_GET, handleQueueClear);');
    expect(ino).toContain('server.on("/queue/stop-clear", HTTP_GET, handleQueueStopClear);');
    expect(ino).toContain('server.on("/ledger/reset", HTTP_GET, handleLedgerReset);');
    expect(ino).toContain('doc["queueState"] = queueStopped ? "stopped" : "running";');
    expect(ino).toContain('JsonArray ledger = doc.createNestedArray("soilLedger")');
  });


  test('validates manual extra watering and exposes pending queue fields', () => {
    expect(ino).toContain('zone must be 1-5 and minutes 1-240');
    expect(ino).toContain('int pendingExtraZone = 0;');
    expect(ino).toContain('int pendingExtraMinutes = 0;');
    expect(ino).toContain('doc["pendingExtraZone"] = pendingExtraZone;');
    expect(ino).toContain('doc["pendingExtraMinutes"] = pendingExtraMinutes;');
  });

  test('validates and persists zone configuration fields', () => {
    expect(ino).toContain('void handleSaveZone()');
    expect(ino).toContain('zone must be 1-5');
    expect(ino).toContain('z.baseMinutes = constrain(server.arg("baseMinutes").toInt(), 1, 240);');
    expect(ino).toContain('z.startHour = constrain(server.arg("startHour").toInt(), 0, 23);');
    expect(ino).toContain('z.startMinute = constrain(server.arg("startMinute").toInt(), 0, 59);');
  });

  test('renders schedule panel using five zones', () => {
    expect(ino).toContain('static const uint8_t DISPLAY_ZONE_COUNT = 5;');
    expect(ino).toContain('for (int i=0;i<DISPLAY_ZONE_COUNT;i++)');
    expect(ino).toContain('int row=i%3,col=i/3;');
    expect(ino).toContain('display.printf("Z%d %d:%02d %dm"');
  });

  test('consumes relay weather and time fields and updates runtime meter with partial refresh', () => {
    expect(ino).toContain('fetchRelayJson("/weather", wdoc)');
    expect(ino).toContain('state.weather.precipitationChancePct');
    expect(ino).toContain('fetchRelayJson("/time", tdoc)');
    expect(ino).toContain('display.setPartialWindow(432, 339, 360, 133);');
    expect(ino).toContain('drawRuntimePanel(432, 339, 360, 133);');
  });

  test('runtime panel idle/running meter semantics match requirements', () => {
    expect(ino).toContain('drawRuntimePanel(432, 339, 360, 133);');
    expect(ino).toContain('display.drawRect(x,y,w,h,GxEPD_BLACK);');
    expect(ino).toContain('display.print("Idle")');
    expect(ino).toContain('display.drawRect(x+8,y+40,w-16,20,GxEPD_BLACK);');
    expect(ino).toContain('display.printf("Running Zone %u", state.run.zone);');
    expect(ino).toContain('display.fillRect(x+9,y+41,(int)((w-18)*r),18,GxEPD_BLACK);');
    expect(ino).toContain('display.printf("Remaining: %um %us", state.run.remainingSeconds/60, state.run.remainingSeconds%60);');
    expect(ino).toContain('display.printf("Finished Zone %u", lastFinishedZone);');
  });

  test('tracks finished-zone state transition and keeps runtime panel dedicated', () => {
    expect(ino).toContain('uint8_t lastFinishedZone = 0;');
    expect(ino).toContain('if (previousRunActive && !state.run.active && previousRunZone > 0) lastFinishedZone = previousRunZone;');
    expect(ino).toContain('if (state.run.active && state.run.zone > 0) { previousRunZone = state.run.zone; lastFinishedZone = 0; }');
  });

  test('admin ui exposes manual display controls and manual watering inputs', () => {
    expect(ino).toContain('Show Schedule');
    expect(ino).toContain('Show News');
    expect(ino).toContain('Show Historic Weather');
    expect(ino).toContain('Resume Auto Rotation');
    expect(ino).toContain("Zone selector <select id='zone'>");
    expect(ino).toContain("Minutes input <input id='minutes' type='number' min='1' max='240'");
    expect(ino).toContain("fetch('/extra?zone='+encodeURIComponent(document.getElementById('zone').value)+'&minutes='+encodeURIComponent(document.getElementById('minutes').value))");
  });

  test('news screen wraps body text and keeps monochrome full-screen styling', () => {
    expect(ino).toContain('display.setCursor(8,25); display.print("Castle Hills Garden News");');
    expect(ino).toContain('display.fillScreen(GxEPD_WHITE);');
    expect(ino).toContain('drawWrappedTextBlock(16, 120, 760, 20, state.gardenNews);');
    expect(ino).toContain('display.drawLine(8,48,792,48,GxEPD_BLACK);');
    expect(ino).toContain('display.drawRect(8,58,784,404,GxEPD_BLACK);');
  });

  test('weather/history screen includes summary, forecast strips, and readable chart frames', () => {
    expect(ino).toContain('display.setCursor(8,25); display.print("Current + Weekly Weather");');
    expect(ino).toContain('display.setCursor(16,72); display.print(state.date);');
    expect(ino).toContain('display.setCursor(680,72); display.print(state.time);');
    expect(ino).toContain('drawGraphFrame(8,285,784,60,"Temp F","90","70","50","Start","End");');
    expect(ino).toContain('drawGraphFrame(8,350,784,60,"Rain in","1.0","0.5","0.0","Start","End");');
    expect(ino).toContain('drawGraphFrame(8,415,784,50,"Sun hrs","12","6","0","Start","End");');
    expect(ino).toContain('display.drawRect(8,176,784,50,GxEPD_BLACK);');
    expect(ino).toContain('display.drawRect(8,230,784,50,GxEPD_BLACK);');
  });


  test('admin ui includes required sections, controls, and news persistence action', () => {
    expect(ino).toContain('<h2>Status</h2>');
    expect(ino).toContain('<h2>Garden Map</h2>');
    expect(ino).toContain('<h2>Zones</h2>');
    expect(ino).toContain('<h2>Full-Screen Garden News</h2>');
    expect(ino).toContain('<h2>Weather History</h2>');
    expect(ino).toContain('Stop / All Off');
    expect(ino).toContain('Sync Weather');
    expect(ino).toContain('Redraw E-Paper');
    expect(ino).toContain('Queue Extra Water');
    expect(ino).toContain('Save Zone');
    expect(ino).toContain('Save Logic');
    expect(ino).toContain('Save News to SD');
    expect(ino).toContain('Download CSV');
    expect(ino).toContain('Clear History');
    expect(ino).toContain('Weather summary:');
    expect(ino).toContain("id='st-weather'");
    expect(ino).toContain("id='st-news'");
    expect(ino).toContain("id='st-run'");
    expect(ino).toContain("id='st-map'");
    expect(ino).toContain("id='st-ledger'");
    expect(ino).toContain("id='st-error'");
    expect(ino).toContain("function fmtMap(s)");
    expect(ino).toContain("function saveZone()");
    expect(ino).toContain("fetch('/saveZone'+q)");
    expect(ino).toContain("if(!r.ok) throw new Error('state request failed: '+r.status);");
    expect(ino).toContain("document.getElementById('st-error').textContent=String(err);");
    expect(ino).toContain('section{border:1px solid #000');
    expect(ino).not.toContain('border-radius:8px');
  });

  test('state endpoint includes weather summary object and queue/display fields', () => {
    expect(ino).toContain('JsonObject weather = doc.createNestedObject("weather")');
    expect(ino).toContain('weather["summary"] = state.weather.summary;');
    expect(ino).toContain('weather["temperatureF"] = state.weather.temperatureF;');
    expect(ino).toContain('weather["sunriseEpoch"] = state.weather.sunriseEpoch;');
    expect(ino).toContain('doc["queueState"] = queueStopped ? "stopped" : "running";');
    expect(ino).toContain('doc["displayMode"] = state.displayMode;');
  });

  test('display redraw flags are raised for config and logic changes', () => {
    expect(ino).toContain('void handleConfigPost()');
    expect(ino).toContain('saveConfig(); forceFullRedraw = true;');
    expect(ino).toContain('void handleSaveLogic(){ forceFullRedraw = true;');
  });

  test('history csv endpoint returns fake weather rows with required schema', () => {
    expect(ino).toContain('epoch,tempF,rainIn,sunlightHours,windMph,weatherCode,reason');
    expect(ino).toContain('1715083200,62.3,0.00,9.4,4.1,1000,baseline');
    expect(ino).toContain('1715169600,65.1,0.12,7.8,6.0,1003,spring-rain');
  });


  test('draws history small-multiple plots using fake CSV rows', () => {
    expect(ino).toContain('static const char* HISTORY_CSV_DATA =');
    expect(ino).toContain('int loadFakeHistory(HistoryRow* rows, int maxRows)');
    expect(ino).toContain('drawHistorySeriesLine(rows, rowCount, 84, 306, 696, 24, true);');
    expect(ino).toContain('drawHistorySeriesRainBars(rows, rowCount, 84, 371, 696, 24);');
    expect(ino).toContain('drawHistorySeriesLine(rows, rowCount, 84, 431, 696, 18, false);');
    expect(ino).toContain('server.send(200,"text/csv",HISTORY_CSV_DATA);');
  });

});
