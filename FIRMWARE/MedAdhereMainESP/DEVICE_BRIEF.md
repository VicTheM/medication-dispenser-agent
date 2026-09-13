# MedAdhere Device Brief

## Two boards, one job split cleanly
- **ESP32-S3 (this firmware)**: WiFi, backend comms, all sensors, LCD, buzzer, LEDs, carousel motor, button.
- **ESP32-CAM (Ai Thinker)**: mic, speaker, camera only. Dumb by design — it just obeys single-letter state commands over Serial2 from the S3. Protocol: `CAM_SERIAL_PROTOCOL.md`. Wire GPIO17/18 (S3) to CAM's TX/RX, common ground, **921600 baud**.

## First-time setup
1. Power on with no saved WiFi credentials → device boots straight into **config mode**.
2. On your phone, join the WiFi network `MedAdhere-Setup-XXXX` (password: `medadhere-setup` by default — **change this on first use**, field is in the form).
3. Browse to the IP shown on the LCD, fill in: home WiFi SSID/password, API base URL, device ID + device secret (from the caregiver dashboard's Device tab), optionally a new setup-network password.
4. Save → device restarts, connects to your WiFi, fetches its schedule, opens its live connection to the backend.

**Config mode later**: single-click the button any time to re-enter config mode (e.g. to move the device to a new WiFi network). It auto-exits after 10 minutes of no activity.

## Normal operation
LCD shows **time to next dose** and its medication names most of the time — that's the default screen.

**At dispense time**: buzzer starts a slow low beep, blue LED pulses, LCD shows "Time for meds!" — this continues until the ultrasonic sensor detects someone within 70cm. The carousel then rotates the right compartment into position and drops the dose without requiring the door to be opened first. The alarm continues while dispensing and until the person opens the door.

**After dispensing**: the LCD asks the person to open the door and pick up the medication. The device watches the tray's IR beam (normally blocked by the closed tray door), keeps the alarm sounding, and stops the alarm once the beam is unblocked. The dose is then reported as picked up; the adherence video is the actual proof of whether it was taken.

**The button**:
- **Long-press** (~1s+): ask the assistant a question out loud. LCD prompts "Listening...", the CAM board records, gets sent to the AI, and the spoken answer plays back through the CAM's speaker.
- **Short-press**: enter config mode (see above).
- **Hold ~8s (`FACTORY_RESET_HOLD_MS`)**: factory reset - wipes all saved WiFi/API/device credentials and carousel calibration from NVS, then reboots straight into first-time config mode. Works from any state, overriding whatever the device was doing.

## Design decisions worth knowing about

**Config portal is WPA2-protected, not TLS.** I initially tried the standard Arduino HTTPS-server library and it doesn't compile against current ESP32 cores (a header it needs, `hwcrypto/sha.h`, has moved) — confirmed by actually compiling it, not assumed. More importantly, even if it did work, self-signed certs throw scary browser warnings during setup, which undermines trust more than it protects. Instead, the setup form is protected by the access point's own WPA2 password — the same approach virtually all consumer WiFi-setup IoT devices use, and arguably more honest than a TLS connection nobody can actually verify anyway. **The connection to your real backend (schedule sync, dispense reporting, WebSocket) is genuine TLS** via `WiFiClientSecure`, which is the connection that actually crosses the internet and matters most.

**TLS trust model**: the firmware currently uses `setInsecure()` for the backend connection (accepts any certificate) rather than pinning your CA. This is a normal MVP simplification but should be hardened before shipping to real patients — pin your backend's certificate/CA via `setCACert()` once you have a stable deployment.

**Carousel homing / drift**: a 28BYJ-48 does 2048 steps/revolution, which doesn't divide evenly by 7 compartments (292.57 steps each). The firmware avoids compounding rounding error by always targeting each compartment's *ideal absolute* step position rather than stepping relative to wherever it last stopped — so error doesn't accumulate across rotations. It still needs **one-time calibration** (a "this is compartment A" reference point) since there's no home sensor in the original hardware list. I added an optional `PIN_HOME_SENSOR` (GPIO16, disabled by default via `#define HAS_HOME_SENSOR`) as a recommended addition — a cheap microswitch or slotted opto-sensor that fires once per revolution — so the device can self-home on every boot instead of trusting persisted position forever. Worth adding before this ships.

**Offline caching is RAM-only**, capped at 20 events/20 telemetry readings (`config.h`). Fine for short outages; a multi-day outage would lose the overflow. If long offline periods are a real scenario, back this with LittleFS instead of plain arrays.

**Video/voice relay blocks the main loop** for the duration of the transfer (bounded, but real — could be tens of seconds). This happens only during the REPORTING and VOICE_QUERY states, where nothing else time-critical needs the CPU, and WS reconnects automatically afterward if it dropped. The clean fix is running networking on its own FreeRTOS task (ESP32 is dual-core) — flagged here as the natural v2 improvement, not done in v1 to keep the design comprehensible.

## Things you didn't mention that I added
1. **The home sensor** above.
2. **Baud rate bump to 921600** — 115200 would make a multi-MB adherence clip take minutes just to cross the wire between boards.
3. **NTP time sync** on boot (`configTime()`) — the schedule engine needs real wall-clock time to know when "now" is.
4. **Per-day dispense de-duplication** (`dispensedToday` flag, reset implicitly by the next schedule refresh) so the same minute-match doesn't fire the dose twice.
