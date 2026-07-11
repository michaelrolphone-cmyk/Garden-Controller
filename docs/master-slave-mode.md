# Master / Slave relay expansion

The relay firmware now uses one image for both controller roles.

## Roles

### Master

The default role preserves the existing controller behavior:

- Local zones use relays 1-5.
- Relay 6 remains the local master valve and spigot supply.
- Local zone and spigot schedules remain in the existing firmware scheduler.
- The master accepts slave heartbeats, displays slave presence, controls remote relays, and owns remote-zone schedules.

### Slave

Slave mode changes all six physical relays into independent timed zone outputs.

- The slave joins the master's access point using its Home/Master WiFi credentials.
- The default master address is `192.168.4.1`.
- The slave keeps its own recovery/admin AP at `192.168.5.1`, avoiding an AP/STA subnet collision.
- The slave reports its identity, station IP, relay states, and remaining run times every five seconds.
- The slave does not execute autonomous schedules and does not synchronize directly with Heroku.
- Timed runs continue to expire locally if communication is interrupted.

## Logical zone numbering

The existing local resource numbering remains stable:

| Logical channel | Resource |
| --- | --- |
| 1-5 | Master local zones |
| 6 | Master spigots / master valve |
| 7-12 | Slave slot 1, physical relays 1-6 |
| 13-18 | Slave slot 2, physical relays 1-6 |
| 19-24 | Slave slot 3, physical relays 1-6 |
| 25-30 | Slave slot 4, physical relays 1-6 |

Slave slot assignments are persisted by device ID, so logical zone numbers remain stable across restarts and temporary disconnections.

## Configuration

Open `/admin` on either device. The role panel is the first section.

For a master:

1. Keep **Device role** set to **Master**.
2. Set a shared master/slave link key.
3. Keep the normal AP settings used by slave devices.

For a slave:

1. Set **Device role** to **Slave**.
2. Give the slave a unique device ID and name.
3. Enter the master's AP SSID and password under **Home/Master WiFi**.
4. Enter the same shared link key used by the master.
5. Leave the master address at `192.168.4.1` unless the master AP uses another address.
6. Save; the device restarts in slave mode.

## Presence and commands

A slave posts heartbeats to:

- `POST /api/slaves/register`
- `POST /api/slaves/heartbeat`

The master sends timed commands to the slave at:

- `POST /api/slave/relay`
- `POST /api/slave/alloff`

The shared link key is included in the JSON body for both directions.

## Remote schedules

The master admin contains a **Slave Zone Schedules** section. Each remote schedule stores:

- logical zone
- start time
- duration
- enabled state
- Sunday-through-Saturday mask

Remote schedules are persisted on the master and dispatched as timed commands. Enabled runs must remain inside the existing 04:00-20:00 watering window. Remote schedules are local to the master firmware and are not currently edited by the Heroku schedule GUI.

## Safety behavior

- Both roles force every relay off during boot.
- Slave relays are always timed and expire locally.
- Slave mode neutralizes legacy local schedules and the special spigot behavior.
- The master's relay 6 behavior is unchanged.
- An offline slave is shown as offline and does not receive commands.
- Factory reset removes role, slave assignments, remote schedules, weekday masks, and spigot schedules; the device returns to Master role with Heroku synchronization off.
