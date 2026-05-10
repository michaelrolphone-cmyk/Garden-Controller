#pragma once
#include <Arduino.h>

struct MapPt {
  float x;
  float y;
};

struct ScreenPt {
  int16_t x;
  int16_t y;
};

struct Poly {
  const MapPt *pts;
  uint8_t n;
  float cx;
  float cy;
};

struct RunItem {
  bool active = false;
  int zoneNumber = 0;          // 1-5, 0 for none
  bool spigot = false;
  long remainingSeconds = -1;
  long durationSeconds = -1;
  uint32_t lastUpdateMs = 0;
};

struct ScheduleItem {
  bool valid = false;
  bool enabled = true;
  int zoneNumber = 0;
  int startHour = 0;
  int startMinute = 0;
  int durationMinutes = 0;
};

struct ZoneSchedule {
  bool loaded = false;
  uint8_t count = 0;
  ScheduleItem slots[6];
};

struct ZoneRgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct TouchPoint {
  bool touched = false;
  int x = 0;
  int y = 0;
};

enum ScreenId {
  SCR_HOME,
  SCR_ZONE_SELECT,
  SCR_DURATION,
  SCR_RUNNING,
  SCR_SCHEDULE,
  SCR_SCHEDULE_EDIT,
  SCR_STOP_CONFIRM,
  SCR_SETUP_ERROR,
  SCR_DIAGNOSTICS
};

enum DurationTarget {
  DUR_ZONE,
  DUR_SPIGOT
};
