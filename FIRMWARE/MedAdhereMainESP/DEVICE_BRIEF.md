# MedAdhere Device Brief

## Two boards, both ESP32-S3, cleanly split
- **Main board (this firmware)**: WiFi, backend comms, all sensors, LCD,
  buzzer relay, LEDs, carousel motor, button.
- **CAM/audio board (separate S3, its own firmware, not covered here)**:
  camera, mic, speaker, and now **its own WiFi connection** — it talks to
  the backend directly for voice queries and adherence capture, rather than
  relaying file bytes through the main board. Link protocol:
  `CAM_SERIAL_PROTOCOL.md`. Wire GPIO17/18 (main board) to the CAM board's
  TX/RX, common ground, **115200 baud**.

This is a deliberate simplification from an earlier framed-binary-protocol
design: the main board now just tells the CAM board "go do the audio task"
or "go do the video task" and waits for a pass/fail byte — everything about
*how* that task gets done (recording, calling the AI service, uploading)
lives entirely on the CAM board.

## First-time setup
1. Power on with no saved WiFi credentials → device boots straight into
   **config mode**.
2. On your phone, join the WiFi network `MedAdhere-Setup-XXXX` (password:
   `medadhere-setup` by default — **change this on first use**, field is in
   the form).
3. Browse to the IP shown on the LCD, fill in: home WiFi SSID/password, API
   base URL, device ID + device secret (from the caregiver dashboard's
   Device tab), **UTC offset in hours** (e.g. `1` for GMT+1 — see below),
   optionally a new setup-network password.
4. Save → device restarts, connects to your WiFi, fetches its schedule,
   opens its live connection to the backend, and pushes its config to the
   CAM board over serial.

**Config mode later**: single-click the button any time to re-enter config
mode. It auto-exits after 10 minutes of no activity.

## Timezone (item you ran into)
The app/backend stores schedule times as the patient's **local wall-clock**
(e.g. "08:00" means 8am where they actually live). The device previously
always synced to raw UTC via `configTime(0, 0, ...)`, so on GMT+1 you had to
schedule everything an hour early to compensate. Fixed: the config portal
now has a **UTC offset (hours)** field, saved to NVS, and the device calls
`configTime(utcOffsetHours * 3600, 0, ...)` so its local time actually
matches the patient's. Set it once per device based on where the patient is
(GMT+1 → enter `1`). This doesn't handle DST automatically — if your region
observes it, you'll need to nudge this by 1 twice a year, or hardcode a
fixed offset that assumes no DST if that's simpler for your deployment.

## Normal operation
LCD shows **time to next dose** and its medication names most of the time.

**At dispense time**: buzzer relay clicks on/off in a slow pattern, blue LED
pulses, LCD shows "Time for meds!" — continues until the ultrasonic sensor
detects someone within 200cm. The instant that happens, the click pattern
speeds up and green LED takes over (a relay can't change pitch like a piezo
buzzer could, so "changes the sound" is now a faster click rate rather than
a tone change), then the carousel rotates the right compartment into
position and drops the dose.

**After dispensing**: the device watches the tray's IR beam (normally
blocked by the closed tray door) and the load cell weight together. Pickup
is confirmed only when *both* the door has opened *and* the tray weight has
changed meaningfully. Confirmed pickup gets a short double-click + green
flash; no pickup within 5 minutes gets a slow triple-click + red flash. This
sensor pair is a fast local signal, not the source of truth — the adherence
capture (CAM board's job now) is the actual proof.

Right after dispensing, the main board sends `'v'` to the CAM board and
waits up to `VIDEO_TIMEOUT_MS` (150s) for a 0/1 result while continuing to
monitor pickup and update the display — see `CAM_SERIAL_PROTOCOL.md`.

**The button**:
- **Long-press** (~1s+): sends `'a'` to the CAM board (ask the assistant a
  question out loud) and waits up to `AUDIO_TIMEOUT_MS` (20s) for 0/1.
- **Short-press**: enter config mode.

## Buzzer is now an active-low relay
`indicators.cpp` drives `PIN_BUZZER` LOW to energize/sound it, HIGH to
silence it (`buzzerOn()`/`buzzerOff()`), replacing the old `tone()`/
`noTone()` piezo control. All the existing patterns (waiting-for-approach,
person-approached, pickup-confirmed, pickup-missed, error, config-mode)
still work the same way structurally — same on/off timing windows, just a
relay click instead of a tone. **If it's a mechanical relay** (not
solid-state), note that `PERSON_APPROACHED` cycles every ~400ms — fine for
solid-state, but worth watching for wear on a mechanical one over time.

## Config handoff to the CAM board
The moment the main board boots, before connecting to its own WiFi, it
sends the CAM board its WiFi SSID/password, API base URL, device ID/secret,
and UTC offset as plain text lines over serial (see
`CAM_SERIAL_PROTOCOL.md` section 1). This means the CAM board can start
connecting to WiFi in parallel rather than waiting on the main board, and
both boards authenticate to the backend as the same logical device.

## Logging
Every state transition, sensor-driven decision, network call result, and
CAM board interaction now goes through a single `logEvent(...)` helper
(timestamped, printf-style, over the main `Serial` debug port at 115200).
Grep your serial monitor for `State:` to follow the state machine, or watch
for `WARNING`/`FAILED` to catch problems. This isn't sent anywhere - it's
local debug output only, not part of the backend telemetry.

## Design decisions still worth knowing about

**Config portal is WPA2-protected, not TLS.** The standard Arduino
HTTPS-server library doesn't compile against current ESP32 cores (confirmed
by actually compiling it, not assumed) and self-signed certs throw browser
warnings that undermine trust more than they protect. The setup form is
instead protected by the access point's own WPA2 password. **The connection
to your real backend is genuine TLS** via `WiFiClientSecure` — that's the
connection that actually crosses the internet and matters most.

**TLS trust model**: `setInsecure()` is used for the backend connection
(accepts any certificate). Normal MVP simplification; pin your CA via
`setCACert()` before shipping to real patients.

**Carousel homing / drift**: a 28BYJ-48 does 2048 steps/revolution, which
doesn't divide evenly by 7 compartments (292.57 steps each). The firmware
targets each compartment's *ideal absolute* step position rather than
stepping relative to wherever it last stopped, so rounding error doesn't
compound across rotations. Still needs **one-time calibration**. An
optional `PIN_HOME_SENSOR` (GPIO16, `#define HAS_HOME_SENSOR` to enable) is
wired in for a cheap microswitch/opto-sensor if you want to add self-homing.

**Offline caching is RAM-only**, capped at 20 events/20 telemetry readings
(`config.h`). Fine for short outages; back it with LittleFS if long offline
periods are a real scenario.

**HX711 calibration factor** (`HX711_CAL_FACTOR` in `config.h`) is a
placeholder — every physical unit needs its own, using known weights
against `get_units()`.

**Open item**: the CAM board's adherence upload needs to know which
`dispense_event_id` to attach to (created server-side when the main board
reports the dispense). The current `'v'` trigger doesn't pass that along —
see the note at the bottom of `CAM_SERIAL_PROTOCOL.md` for two ways to close
that gap before this goes further.
