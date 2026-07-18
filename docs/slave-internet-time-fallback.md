# Slave Internet Time Fallback

Slave controllers execute their synchronized schedules locally. They normally obtain authoritative time from the master heartbeat, but they also support an independent Internet time path so a master outage cannot leave a cold-booted slave without a valid clock.

## WiFi profiles

A slave stores two station profiles:

1. **Home/Master WiFi** — the master's access point. This remains the preferred connection and carries heartbeats, schedule revisions, relay state, and run journals.
2. **Fallback Internet WiFi** — an Internet-capable network used only to retrieve time when the master is unavailable.

The fallback SSID must be different from the master AP SSID. The slave's recovery access point remains active at `192.168.5.1` while the station radio switches between the two profiles.

## Clock recovery sequence

1. The slave waits 45 seconds for the master after losing contact.
2. If the system clock is invalid, or the configured Internet resync interval has elapsed during a prolonged master outage, it temporarily joins the fallback Internet WiFi.
3. It sends a direct NTP request to the primary server and then the secondary server if needed.
4. The clock is accepted only after a valid NTP UDP response containing a plausible Unix epoch is received.
5. The slave sets its system clock, persists the successful Internet-sync timestamp, and reconnects to the master AP.
6. Local schedules continue to use the synchronized clock while the master remains unavailable.

Defaults:

- Primary NTP server: `pool.ntp.org`
- Secondary NTP server: `time.nist.gov`
- Internet resync interval during a master outage: 6 hours
- Retry interval after a failed Internet/master cycle: 5 minutes

All values are configurable from `/admin`.

## Schedule recovery after time synchronization

Slave schedules are fixed start/end intervals rather than one-shot start events. As soon as the clock becomes valid, the slave compares current local time with every persisted schedule:

- If current time is inside an interval, the relay is turned on immediately.
- The relay timer is set only to the seconds remaining before the original end.
- Repeated reconciliation does not restart or extend the interval.
- If the interval has already ended, it is not replayed.
- At the end time, a schedule-controlled relay is turned off.

This behavior also applies after reboot and after a forward or backward clock correction.

## Status and fault reporting

The slave reports the following to the master after reconnecting:

- Whether the clock is valid
- Current time source (`master`, `internet-ntp`, `retained`, or `unsynchronized`)
- Last successful Internet NTP epoch
- Current fallback status

The master admin page displays this status per slave. An invalid clock is shown explicitly as **INVALID** rather than silently presenting schedules as operational.

The slave's local admin page also reports critical states, including:

- Fallback Internet WiFi not configured while the clock is invalid
- Internet WiFi connection failure
- Both NTP servers failing
- Master and Internet time both unavailable

## Safety behavior

- Active relay timers remain local and continue through WiFi switching.
- The master never assumes that a slave schedule ran.
- Relay state and remaining time remain heartbeat-confirmed.
- Start and stop events are retained until the master acknowledges them.
- Schedule snapshots remain persisted on the slave during all network transitions.
- A successful Internet sync begins a new resynchronization interval.
- Factory reset clears fallback WiFi credentials, NTP settings, and last-sync metadata.
