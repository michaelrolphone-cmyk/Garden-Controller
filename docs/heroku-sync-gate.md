# Heroku synchronization safety gate

The relay firmware defaults Heroku synchronization to **Off** until it is explicitly enabled from the captive-portal admin interface.

## First boot behavior

On the first boot after installing firmware with this feature, the ESP32 writes these values to the existing `relay6` Preferences namespace:

- `remoteEn = false`
- `syncManaged = true`

The gate is applied from `initVariant()` after ESP32 NVS initialization and before the firmware `setup()` function. This prevents the remote synchronization task from contacting Heroku before the setting is applied.

A factory reset clears `syncManaged`. On the next boot, synchronization therefore defaults to Off again.

## Activating synchronization

1. Connect to the controller access point.
2. Open the captive portal and select **Admin**.
3. In **Remote API Settings**, turn on **Remote API enabled**.
4. Select **Save Device Settings** or **Save WiFi Settings**. Both buttons save the complete admin configuration, including the synchronization switch.

After it has been saved, the setting persists across normal restarts and firmware boots.

## Disabled behavior

When the switch is Off, `remoteReady()` remains false. The controller continues to run local zone and spigot schedules, serve the captive portal, use its configured local time, and fetch its weather data, but it does not:

- poll Heroku for commands;
- publish relay state;
- publish controller state or sensor data;
- publish zone or spigot schedules;
- download spigot schedules from Heroku.

The local controller remains fully usable while synchronization is disabled.
