# Offline-resilient slave scheduling

The master remains the configuration authority, but each slave is the execution authority for its own six physical relays.

## Schedule synchronization

1. The master stores one versioned schedule collection.
2. Each heartbeat includes the schedule revision currently applied by the slave.
3. When revisions differ, the master returns a complete schedule snapshot for that slave's six logical zones.
4. The slave validates the snapshot, persists it in ESP32 Preferences, and reports the applied revision on its next heartbeat.
5. The master admin panel shows the target and applied revisions separately.

A disconnected slave continues to evaluate its persisted schedules using its local clock. Heartbeats copy the master's valid epoch to the slave so the slave clock remains aligned while connected. If the device has never obtained valid time, automatic runs remain disabled rather than running at an unknown time.

## Interval execution

Each schedule defines an absolute local-time start and end. The slave continuously reconciles every relay with those intervals:

- inside an enabled interval, the relay must be on;
- the timer contains only the time remaining until the original end;
- outside the interval, a schedule-controlled relay must be off;
- repeated evaluations do not extend the end time;
- after reboot or clock recovery, an interval still in progress resumes for only its remaining portion;
- an interval that already ended is never replayed later.

## Run truth and reporting

The master does not start slave schedule timers and does not mark a slave zone active when a schedule becomes due. The slave:

- starts the physical relay locally;
- owns the interval deadline;
- writes an immutable start event to a persistent run journal;
- writes a separate stop event when the interval ends or the relay is stopped;
- keeps unacknowledged events through network interruptions and restarts;
- sends the journal in heartbeats until the master acknowledges it.

The heartbeat also carries the current relay state and remaining time. This current state is authoritative. When a slave is offline, the master reports the current state as unknown and retains the last reported state only as historical context.

## Recovery behavior

- A schedule change made while a slave is offline remains pending on the master.
- The slave continues running its previously applied schedule revision.
- On reconnect, the slave first reports actual current relay state and any queued run events.
- The master acknowledges the run journal and returns the latest schedule snapshot if required.
- The slave applies the new revision and immediately reconciles the current interval.

## Preferences namespaces

| Namespace | Contents |
|---|---|
| `r6masterdist` | Master schedule collection and target revision |
| `r6slavesched` | Slave-local schedule snapshot, revision, assignment, and interval markers |
| `r6runlog` | Unacknowledged start/stop events and active-run metadata |
| `r6observed` | Master's latest acknowledged run observations and slave-applied revisions |

## User interface semantics

For an online slave, the timer is calculated from the latest heartbeat-confirmed remaining time and the age of that heartbeat.

For an offline slave, the UI displays `UNKNOWN` rather than continuing to claim that the last reported ON or OFF state is current. The last confirmed state and actual run report remain visible.
