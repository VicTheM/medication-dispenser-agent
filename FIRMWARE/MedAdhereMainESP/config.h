/*
  config.h - pin map, timing constants, and compile-time configuration for
  the MedAdhere main controller (ESP32-S3).

  Pin choices follow the same numbering already validated in
  PERIPHERAL_TEST_S3.ino, plus two additions explained in DEVICE_BRIEF.md:
    - GPIO16 : optional carousel home sensor (NOT in the original test
               sketch - see brief, section "Carousel homing")
    - GPIO17/18 : now actually used, for Serial2 to the ESP32-CAM board
*/
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------
// Indicators (shared status LEDs)
// ---------------------------------------------------------------------
#define PIN_LED_GREEN     4
#define PIN_LED_RED       5
#define PIN_LED_BLUE      6

// ---------------------------------------------------------------------
// Ultrasonic (HC-SR04) - approach detection
// ---------------------------------------------------------------------
#define PIN_TRIG          7
#define PIN_ECHO          15
#define SOUND_SPEED_CM_US 0.0343f
#define APPROACH_RANGE_CM 200.0f   // "within range" threshold from the brief

// ---------------------------------------------------------------------
// Carousel stepper (28BYJ-48 + ULN2003) - selects which compartment
// aligns with the dispense chute
// ---------------------------------------------------------------------
#define PIN_STEP_IN1      8
#define PIN_STEP_IN2      9
#define PIN_STEP_IN3      10
#define PIN_STEP_IN4      11
#define STEPS_PER_REV     2048
#define NUM_COMPARTMENTS  8
// 2048 / 7 is not a whole number (292.57) - see DEVICE_BRIEF.md "Carousel
// homing" for why this matters and how drift is corrected.
#define PIN_HOME_SENSOR   16       // optional; #define HAS_HOME_SENSOR to enable
// #define HAS_HOME_SENSOR

// ---------------------------------------------------------------------
// LCD (16x2 I2C)
// ---------------------------------------------------------------------
#define LCD_ADDRESS       0x27
#define LCD_COLS          16
#define LCD_ROWS          2
#define PIN_I2C_SDA       12
#define PIN_I2C_SCL       13

// ---------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------
#define PIN_BUZZER        14

// ---------------------------------------------------------------------
// Config / voice button (short press = config portal, long press = ask AI)
// ---------------------------------------------------------------------
#define PIN_BUTTON        21
#define LONG_PRESS_MS      900
#define FACTORY_RESET_HOLD_MS (8UL * 1000UL)  // hold button this long to wipe NVS and return to first-time setup

// ---------------------------------------------------------------------
// IR "laser" beam-break sensor at the picking-tray door
// ---------------------------------------------------------------------
#define PIN_IR_BEAM       38

// ---------------------------------------------------------------------
// HX711 + load cell (picking tray)
// ---------------------------------------------------------------------
#define PIN_HX711_DOUT    39
#define PIN_HX711_SCK     40
#define HX711_CAL_FACTOR  -7050.0f   // MUST be recalibrated per unit - see brief
#define TRAY_PICKUP_DELTA_G 2.0f     // grams of change that counts as "picked up"

// ---------------------------------------------------------------------
// Serial2 link to the ESP32-CAM (Ai Thinker) board
// ---------------------------------------------------------------------
#define PIN_CAM_RX        17   // wire to CAM board's TX
#define PIN_CAM_TX        18   // wire to CAM board's RX
#define CAM_SERIAL_BAUD   921600  // 115200 is too slow for video-sized transfers - see CAM_SERIAL_PROTOCOL.md

// ---------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------
#define TELEMETRY_INTERVAL_MS      3000UL                   // 3s
#define ALERT_MAX_WAIT_MS          (5UL * 60UL * 1000UL)   // give up ringing after 5 min
#define PICKUP_MONITOR_MS          (2UL * 60UL * 1000UL)   // watch for pickup for 2 min
#define ADHERENCE_VIDEO_MS         (10UL * 1000UL)        // 10 secs
#define WIFI_CONNECT_TIMEOUT_MS    20000UL
#define WS_RECONNECT_INTERVAL_MS   5000UL
#define VOICE_MAX_RECORD_MS        (15UL * 1000UL)

// ---------------------------------------------------------------------
// Config portal (local setup access point)
// ---------------------------------------------------------------------
#define CONFIG_AP_SSID_PREFIX  "MedAdhere-Setup-"   // + short device id suffix
#define CONFIG_AP_PASSWORD_DEFAULT "medadhere-setup"  // change on first boot - see brief
#define CONFIG_PORTAL_TIMEOUT_MS (10UL * 60UL * 1000UL)  // auto-exit after 10 min idle

// ---------------------------------------------------------------------
// Offline cache sizing (RAM-backed - see brief for the tradeoff)
// ---------------------------------------------------------------------
#define OFFLINE_CACHE_MAX_EVENTS   20
#define OFFLINE_CACHE_MAX_TELEMETRY 20

// ---------------------------------------------------------------------
// NVS namespace/keys
// ---------------------------------------------------------------------
#define NVS_NAMESPACE      "medadhere"
#define NVS_KEY_WIFI_SSID  "wifi_ssid"
#define NVS_KEY_WIFI_PASS  "wifi_pass"
#define NVS_KEY_API_BASE   "api_base"
#define NVS_KEY_DEVICE_UID "dev_uid"
#define NVS_KEY_DEV_SECRET "dev_secret"
#define NVS_KEY_CAROUSEL_POS "car_pos"
#define NVS_KEY_AP_PASSWORD  "ap_pass"

#define DEFAULT_API_BASE   "https://medication-dispenser-agent.onrender.com"
