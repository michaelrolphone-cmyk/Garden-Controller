const fs = require('fs');
const path = require('path');

function remainingSeconds(startSecond, durationSeconds, currentSecond) {
  const endSecond = startSecond + durationSeconds;
  return currentSecond >= startSecond && currentSecond < endSecond
    ? endSecond - currentSecond
    : 0;
}

describe('absolute schedule interval reconciliation', () => {
  const source = fs.readFileSync(
    path.join(
      __dirname,
      '..',
      'mcu',
      'relay',
      'ZZZZZZZScheduleIntervalReconciliation.ino'
    ),
    'utf8'
  );

  test('a 06:00-06:20 interval runs only until its original end', () => {
    const start = 6 * 3600;
    const duration = 20 * 60;
    expect(remainingSeconds(start, duration, 5 * 3600 + 59 * 60)).toBe(0);
    expect(remainingSeconds(start, duration, 6 * 3600)).toBe(20 * 60);
    expect(remainingSeconds(start, duration, 6 * 3600 + 10 * 60)).toBe(10 * 60);
    expect(remainingSeconds(start, duration, 6 * 3600 + 19 * 60 + 59)).toBe(1);
    expect(remainingSeconds(start, duration, 6 * 3600 + 20 * 60)).toBe(0);
    expect(remainingSeconds(start, duration, 6 * 3600 + 21 * 60)).toBe(0);
  });

  test('repeated reconciliation cannot restore the full configured duration', () => {
    const start = 6 * 3600;
    const duration = 20 * 60;
    expect(remainingSeconds(start, duration, 6 * 3600 + 5 * 60)).toBe(15 * 60);
    expect(remainingSeconds(start, duration, 6 * 3600 + 10 * 60)).toBe(10 * 60);
    expect(remainingSeconds(start, duration, 6 * 3600 + 15 * 60)).toBe(5 * 60);
  });

  test('firmware uses remaining interval time for both master and slave', () => {
    expect(source).toContain('return end - current;');
    expect(source).toContain('startRunSeconds(zoneIndex, remainingSeconds, false)');
    expect(source).toContain('startSpigotRunSeconds(remainingSeconds)');
    expect(source).toContain('reliableStartSlaveRelay');
    expect(source).toContain('meshSlaveRuns[relayIndex].durationMs = remainingMs');
    expect(source).toContain('reliableStopSlaveRelay(relay, "completed")');
  });

  test('ended intervals are not queued or replayed', () => {
    expect(source).not.toContain('catch-up queue');
    expect(source).not.toContain('CATCHUP_WINDOW');
    expect(source).not.toContain('"catchup"');
  });
});
