#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------
// Top-level device state machine
// ---------------------------------------------------------------------
enum class DeviceState {
  BOOT,
  WIFI_CONNECTING,
  CONFIG_PORTAL,
  NORMAL,            // idle, showing time-to-next-dose on the LCD
  ALERTING,          // buzzing, waiting for the patient to approach
  DISPENSING,        // rotating carousel + releasing
  MONITOR_PICKUP,    // watching laser + weight to confirm pickup
  REPORTING,         // waiting on the CAM board's video-task result
  VOICE_QUERY,       // long-press triggered: CAM board handles the AI round trip
};

// ---------------------------------------------------------------------
// One compartment's schedule entry, as received from update_schedule
// ---------------------------------------------------------------------
struct CompartmentSlot {
  bool active = false;
  char dispenseTime[6] = "";     // "HH:MM"
  char frequency[16] = "daily";  // daily | specific_days | as_needed
  uint8_t daysOfWeekMask = 0;    // bit0=mon .. bit6=sun, only for specific_days
  char scheduleId[40] = "";
  char medicationNames[80] = ""; // comma-joined, for display/telemetry only
  bool dispensedToday = false;   // reset at local midnight
};

// A single queued item while the device is offline.
struct CachedDispenseEvent {
  char compartment;
  char status[8];
  char scheduledTime[6];
  time_t dispensedAt;
};

struct CachedTelemetry {
  time_t reportedAt;
  float batteryLevel;
  char trayState[8];
  bool personDetected;
  int wifiRssi;
  uint32_t uptimeSeconds;
};
