const fs = require('fs');
const path = require('path');

describe('firmware local admin/mobile api integration', () => {
  const ino = fs.readFileSync(path.join(__dirname, '..', 'mcu', 'relay', 'GardenSimpleRelay6.ino'), 'utf8');

  test('exposes config/features/schedules api routes', () => {
    expect(ino).toContain('server.on("/api/features", HTTP_GET, handleApiFeatures);');
    expect(ino).toContain('server.on("/api/config", HTTP_GET, handleApiConfigGet);');
    expect(ino).toContain('server.on("/api/config", HTTP_POST, handleApiConfigSet);');
    expect(ino).toContain('server.on("/api/schedules", HTTP_POST, handleApiScheduleSet);');
    expect(ino).toContain('server.on("/time", HTTP_GET, handleTimeGet);');
    expect(ino).toContain('server.on("/weather", HTTP_GET, handleWeatherGet);');
  });

  test('exposes relay time and weather payload fields required by e-ink controller', () => {
    expect(ino).toContain('doc["epoch"] = (uint32_t)nowEpoch;');
    expect(ino).toContain('doc["synced"] = nowEpoch > 1700000000;');
    expect(ino).toContain('doc["summary"] = weatherSnapshot.summary;');
    expect(ino).toContain('doc["condition"] = weatherSnapshot.condition;');
    expect(ino).toContain('doc["temperatureF"] = weatherSnapshot.temperatureF;');
    expect(ino).toContain('doc["rainIn"] = weatherSnapshot.rainIn;');
    expect(ino).toContain('doc["windMph"] = weatherSnapshot.windMph;');
    expect(ino).toContain('doc["windDeg"] = weatherSnapshot.windDeg;');
    expect(ino).toContain('doc["windDirection"] = weatherSnapshot.windDirection;');
    expect(ino).toContain('doc["humidityPct"] = weatherSnapshot.humidityPct;');
    expect(ino).toContain('doc["dewPointF"] = weatherSnapshot.dewPointF;');
    expect(ino).toContain('doc["precipitationChancePct"] = weatherSnapshot.precipitationChancePct;');
    expect(ino).toContain('doc["sunlightHours"] = weatherSnapshot.sunlightHours;');
    expect(ino).toContain('doc["predictedPrecipIn"] = weatherSnapshot.predictedPrecipIn;');
    expect(ino).toContain('doc["sunriseEpoch"] = weatherSnapshot.sunriseEpoch;');
    expect(ino).toContain('doc["sunsetEpoch"] = weatherSnapshot.sunsetEpoch;');
    expect(ino).toContain('doc["weatherCode"] = weatherSnapshot.weatherCode;');
    expect(ino).toContain('doc["lastWeatherMs"] = weatherSnapshot.lastWeatherMs;');
    expect(ino).toContain('https://api.open-meteo.com/v1/forecast?latitude=');
    expect(ino).toContain('current=temperature_2m,relative_humidity_2m,dew_point_2m,precipitation,rain,weather_code,wind_speed_10m,wind_direction_10m');
    expect(ino).toContain('hourly=precipitation_probability,sunshine_duration');
    expect(ino).toContain('daily=precipitation_sum,sunrise,sunset,precipitation_probability_max');
    expect(ino).toContain('temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch&timezone=auto&timeformat=unixtime');
    expect(ino).toContain('configTzTime(gardenPosixTimeZone, "pool.ntp.org", "time.nist.gov");');
    expect(ino).toContain('char gardenPosixTimeZone[64] = "MST7MDT,M3.2.0,M11.1.0";');
  });

  test('status payload also exposes weather fields used by e-ink fallback parsing', () => {
    expect(ino).toContain('doc["summary"] = weatherSnapshot.summary;');
    expect(ino).toContain('doc["condition"] = weatherSnapshot.condition;');
    expect(ino).toContain('doc["temperatureF"] = weatherSnapshot.temperatureF;');
    expect(ino).toContain('doc["rainIn"] = weatherSnapshot.rainIn;');
    expect(ino).toContain('doc["windMph"] = weatherSnapshot.windMph;');
    expect(ino).toContain('doc["windDeg"] = weatherSnapshot.windDeg;');
    expect(ino).toContain('doc["windDirection"] = weatherSnapshot.windDirection;');
    expect(ino).toContain('doc["humidityPct"] = weatherSnapshot.humidityPct;');
    expect(ino).toContain('doc["dewPointF"] = weatherSnapshot.dewPointF;');
    expect(ino).toContain('doc["precipitationChancePct"] = weatherSnapshot.precipitationChancePct;');
    expect(ino).toContain('doc["sunlightHours"] = weatherSnapshot.sunlightHours;');
    expect(ino).toContain('doc["predictedPrecipIn"] = weatherSnapshot.predictedPrecipIn;');
    expect(ino).toContain('doc["sunriseEpoch"] = weatherSnapshot.sunriseEpoch;');
    expect(ino).toContain('doc["sunsetEpoch"] = weatherSnapshot.sunsetEpoch;');
    expect(ino).toContain('doc["weatherCode"] = weatherSnapshot.weatherCode;');
    expect(ino).toContain('doc["lastWeatherMs"] = weatherSnapshot.lastWeatherMs;');
  });

  test('admin ui includes direct relay on/off controls and api schedule manager', () => {
    expect(ino).toContain("/api/relay?zone=");
    expect(ino).toContain('Turn On');
    expect(ino).toContain('Turn Off');
    expect(ino).toContain('Castle Hills Garden Manager (Firmware Local)');
    expect(ino).toContain('Garden Zone Map');
    expect(ino).toContain('Schedule Timeline');
    expect(ino).toContain("setInterval(()=>refresh(false),1000)");
    expect(ino).toContain("<b>Spigots</b>");
    expect(ino).toContain("const spigotsActive=!!(s.spigotRun&&s.spigotRun.active);");
    expect(ino).toContain("id=\"m-spigots\"");
    expect(ino).toContain("cmd('/api/spigots-run?minutes='+encodeURIComponent(document.getElementById('m-spigots').value))");
  });

  test('admin ui maps zone activity, color-codes map zones, and provides schedule CRUD inputs', () => {
    expect(ino).toContain('function colorByChannel(zoneColors, ch)');
    expect(ino).toContain("function toRgb(c){if(!c)return '0,0,0';const rgb=c.rgb||c;");
    expect(ino).toContain('data-zone="1"');
    expect(ino).toContain('s.zoneColors');
    expect(ino).toContain('zoneChannel(z)');
    expect(ino).toContain("d.zoneName||('Zone '+d.zone)");
    expect(ino).toContain('addScheduleRow(');
    expect(ino).toContain('saveSchedules()');
    expect(ino).toContain("#adminSchedRows .actions");
    expect(ino).toContain('lastScheduleKey');
    expect(ino).toContain('scheduleEditorBusy()');
    expect(ino).toContain("style='max-width:84px'");
    expect(ino).toContain("style='max-width:62px'");
    expect(ino).toContain("<input type='hidden' value='on' title='Enabled'>");
    expect(ino).toContain("<button class='danger' onclick=\"this.parentElement.remove()\">Delete</button>");
    expect(ino).toContain('const shouldRedrawSchedules=forceScheduleRedraw||nextScheduleKey!==lastScheduleKey');
    expect(ino).toContain('class="relay-card ${z.relayOn?\'active-zone\':\'\'}"');
    expect(ino).toContain('class="relay-row"');
    expect(ino).toContain('class="actions relay-controls"');
    expect(ino).toContain('.relay-card.active-zone');
    expect(ino).toContain('doc.createNestedArray("zoneColors")');
    expect(ino).toContain('updateZoneRgbLed()');
    expect(ino).toContain('millis() / 1000UL');
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
    expect(ino).toContain('Save WiFi Settings');
    expect(ino).toContain('grid-template-columns:90px 1fr');
    expect(ino).toContain('renderTimeline(buildTimelineRows(schedules), s.zoneColors||[])');
    expect(ino).not.toContain(">Test Buzzer<");
    expect(ino).toContain('flex-wrap:wrap');
    expect(ino).toContain("function hydrateConfig(s){document.getElementById('apSsid').value=s.apSsid||''");
    expect(ino).toContain("document.getElementById('remoteApiBase').value=s.remoteApiBase||''");
    expect(ino).toContain("document.getElementById('remoteDeviceId').value=s.remoteDeviceId||''");
    expect(ino).toContain("document.getElementById('remoteApiKey').value=s.remoteApiKey||''");
    expect(ino).toContain("cmd('/api/manual-run?zone=${z.zone}&minutes='+encodeURIComponent(document.getElementById('m${z.zone}').value))");
    expect(ino).toContain("rg.innerHTML=spigotCard+zoneCards");
    expect(ino).toContain('<div class="actions header-actions"><button onclick="syncTime()">Sync Phone Time</button><button onclick="cmd(\'/api/alloff\')">Turn Off All Relays</button></div>');
    expect(ino).toContain('doc["staSsid"] = staSsid;');
    expect(ino).toContain('doc["remoteApiBase"] = remoteApiBase;');
  });

  test('config api still accepts and persists wifi and remote fields', () => {
    expect(ino).toContain('void handleApiConfigSet()');
    expect(ino).toContain('if (doc["staSsid"].is<const char*>()) strlcpy(staSsid, doc["staSsid"], sizeof(staSsid));');
    expect(ino).toContain('if (doc["staPass"].is<const char*>()) strlcpy(staPass, doc["staPass"], sizeof(staPass));');
    expect(ino).toContain('if (doc["remoteApiBase"].is<const char*>()) strlcpy(remoteApiBase, doc["remoteApiBase"], sizeof(remoteApiBase));');
    expect(ino).toContain('if (doc["remoteDeviceId"].is<const char*>()) strlcpy(remoteDeviceId, doc["remoteDeviceId"], sizeof(remoteDeviceId));');
    expect(ino).toContain('if (doc["remoteApiKey"].is<const char*>()) strlcpy(remoteApiKey, doc["remoteApiKey"], sizeof(remoteApiKey));');
    expect(ino).toContain('if (strlen(staSsid) > 0) connectSta(false);');
    expect(ino).toContain('saveConfig();');
  });


  test('uses WS2812B serial RGB status LED on pin 38', () => {
    expect(ino).toContain('#include <Adafruit_NeoPixel.h>');
    expect(ino).toContain('static const uint8_t RGB_LED_PIN = 38;');
    expect(ino).toContain('static const uint8_t RGB_LED_BRIGHTNESS = 16;');
    expect(ino).toContain('Adafruit_NeoPixel statusPixel(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);');
    expect(ino).toContain('statusPixel.setBrightness(RGB_LED_BRIGHTNESS);');
    expect(ino).toContain('statusPixel.setPixelColor(0, statusPixel.Color(r, g, b));');
    expect(ino).not.toContain('RGB_LED_PIN_R');
    expect(ino).not.toContain('configureRgbPwmChannel');
  });

});
