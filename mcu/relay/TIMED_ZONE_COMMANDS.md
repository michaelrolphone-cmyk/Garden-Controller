# Timed Zone Commands

This firmware matches Garden-Controller PR #22.

## Server command

```json
{
  "command": {
    "id": "server-command-id",
    "channel": 1,
    "action": "on",
    "durationSeconds": 900
  }
}
```

## Behavior

Channels 1-5 are scheduled zones.

For channels 1-5:

```text
action on     -> start timed manual run
action off    -> stop the zone/current run
action toggle -> stop if active, otherwise timed manual run
```

Channel 6 is master valve/spigots:

```text
action on     -> start timed spigot/master-valve run
action off    -> stop spigot run
action toggle -> stop if active, otherwise timed spigot run
```

## Defaults

If `durationSeconds` is missing or invalid:

```text
900 seconds / 15 minutes
```

The timer runs locally on the ESP32 so weak WiFi or server disconnects cannot leave the valve waiting for a delayed OFF command.
