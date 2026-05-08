# Garden Mesh Protocol v0.1

## Message envelope

```json
{
  "id": "msg-001",
  "type": "zone.run.start",
  "source": "display-master",
  "target": "relay-controller-AABBCCDDEE",
  "createdAt": 1777901400,
  "expiresAt": 1777901700,
  "configVersion": 42,
  "payload": {
    "zones": [1, 2],
    "durationSeconds": 900
  },
  "requiresAck": true
}
```

## Event envelope

```json
{
  "id": "relay-controller-AABBCCDDEE-1777901400-1",
  "type": "zone.run.started",
  "source": "relay-controller-AABBCCDDEE",
  "deviceType": "relay-controller",
  "createdAt": 1777901400,
  "configVersion": 42,
  "payload": {
    "zone": 1,
    "durationSeconds": 900
  }
}
```

## Config conflict rule

```text
dirty local config:
  reject incoming config until local config is recorded by master

incoming version > local version:
  apply if not dirty

incoming version == local version and hash matches:
  in sync

incoming version == local version and hash differs:
  conflict

incoming version < local version:
  reject stale config
```
