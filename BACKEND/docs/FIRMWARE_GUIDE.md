# Firmware Developer Guide — MedAdhere Hardware Integration

This covers everything the ESP32 (or equivalent) firmware needs to talk to
the MedAdhere backend: connecting, getting your schedule, reporting
dispenses/video/telemetry, handling offline caching, and the voice pipeline.

Every device has two credentials, generated when a caregiver registers the
unit to a patient:
- `device_uid` — the serial/QR code printed on the unit
- `device_secret` — a long random token

Keep `device_secret` in NVS/flash, never hardcode or log it in plaintext.

## 1. Connection sequence

1. Connect to Wi-Fi.
2. Open the persistent WebSocket: control-plane traffic only (schedule
   pushes, commands, acks). Keep this open for the device's whole runtime;
   reconnect with backoff if it drops.
3. Use the separate HTTPS endpoints below for everything else (dispense
   events, adherence video, telemetry, offline sync, voice queries) — these
   need reliable, individually-retriable delivery (especially the ~2-minute
   video), which a single WS stream isn't a good fit for.

### WebSocket

```
wss://<host>/devices/ws?device_uid=<DEVICE_UID>&secret=<DEVICE_SECRET>
```

On successful connect, the server immediately sends the full schedule:

```json
{
  "type": "update_schedule",
  "patient_id": "...",
  "timezone": "America/New_York",
  "compartments": {
    "A": {
      "schedule_id": "...",
      "dispense_time": "08:00",
      "frequency": "daily",
      "days_of_week": null,
      "start_date": null,
      "end_date": null,
      "medications": [{"id": "...", "name": "Metformin", "dosage": "500mg"}]
    },
    "B": null,
    "C": null, "D": null, "E": null, "F": null, "G": null
  }
}
```
Store this locally (flash/RAM) and dispense autonomously from it — don't
require a live connection to fire a scheduled dose. A `null` compartment
means it's empty/unused.

The server can push these message types at any time, unprompted:

| `type` | Meaning | Your response |
|---|---|---|
| `update_schedule` | Full schedule replace (same shape as above) | Overwrite local schedule; reply with an `ack` (see below) |
| `manual_dispense` | `{"type":"manual_dispense","command_id":"...","payload":{"compartment":"C"}}` | Dispense that compartment now |
| `restart` | `{"type":"restart","command_id":"..."}` | Reboot |
| `sync` | Ask you to push offline-cached data now | Call `POST /devices/sync-offline` (see §4) |
| `configure` | `{"type":"configure","command_id":"...","payload":{...}}` | Apply whatever config keys are in `payload` (defined as your firmware grows) |

For every command carrying a `command_id`, send an ack so the backend can
mark it delivered/confirmed:
```json
{"type": "ack", "command_id": "<id from the command>"}
```
An optional `{"type":"heartbeat"}` message from the device refreshes
`last_seen_at` between telemetry reports if you want tighter online/offline
detection; not required if you're already sending telemetry frequently.

If the socket drops, the backend marks the device `offline` immediately.
Reconnect with exponential backoff; on reconnect you'll automatically get a
fresh `update_schedule` push, so no special resync handshake is needed for
the schedule itself — just also call `POST /devices/sync-offline` (§4) to
flush anything you cached while disconnected.

## 2. HTTPS auth

Every HTTPS endpoint below requires these two headers:
```
X-Device-Id: <device_uid>
X-Device-Secret: <device_secret>
```

## 3. Reporting a dispense event

Immediately after a successful (or failed) dispense:

```
POST /devices/dispense-event
{
  "compartment": "A",
  "status": "success",              // success | failed | skipped | manual
  "scheduled_time": "08:00",        // the time it was scheduled for, if applicable
  "dispensed_at": "2026-08-01T08:00:05",  // your local RTC time, ISO 8601
  "was_offline_cached": false
}
```
Then record the 2-minute adherence video and upload it, tagged with the
`id` returned from this call:

```
POST /devices/adherence-video      (multipart/form-data)
  dispense_event_id: "<id from above>"
  duration_seconds: 120
  video: <file bytes>
```

## 4. Telemetry & offline caching

Send periodic telemetry (suggested: every 1–5 minutes, or on any state
change):
```
POST /devices/telemetry
{
  "current_compartment": "A",
  "motor_status": "idle",
  "sensor_status": "ok",
  "person_detected": true,
  "tray_state": "full",           // full | low | empty
  "battery_level": 88.5,
  "wifi_rssi": -55,
  "uptime_seconds": 123456,
  "dispense_history": [ ... ]     // optional: your local recent-history snapshot
}
```

**While offline** (no Wi-Fi / can't reach the backend): keep dispensing on
schedule from your locally stored compartment data, and queue dispense
events + telemetry in flash/RAM. Once connectivity returns, flush the queue
in one call:

```
POST /devices/sync-offline
{
  "dispense_events": [ { ...same shape as §3... }, ... ],
  "telemetry": [ { ...same shape as above... }, ... ]
}
```
Dispense events sent this way are automatically flagged
`was_offline_cached: true` server-side once you also set that field on each
item (do set it explicitly to `true` for cached events).

## 5. Voice queries — **use a file upload, not a stream**

The user asks a question out loud; the device records it, then needs a
spoken answer back. **Record to a file and upload it whole — don't stream.**
Reasons this is the right call for an ESP32:

- The upstream AI endpoint (`/voice/ask` on the Ally Healthwise service)
  only accepts a complete `multipart/form-data` audio file — there's no
  streaming/chunked variant to forward into even if you wanted to.
- A short spoken question is a few seconds of audio — at typical
  speech-optimized rates (e.g. 16kHz mono PCM/WAV) that's well under a few
  hundred KB, trivial for the ESP32's SPIFFS/SD/PSRAM to buffer, and a single
  HTTPS POST is far simpler and more robust on a flaky Wi-Fi link than
  managing a live duplex audio stream, backpressure, and reconnect-mid-stream
  logic on constrained hardware.
- File upload gives you natural retry semantics (just re-POST the file) if
  the request fails, which a stream doesn't.

So: record until silence/button-release, save as WAV (or MP3 if your codec
supports it), then:

```
POST /devices/voice-query          (multipart/form-data)
  audio: <recorded file>
  audio_format: "wav"              // or "mp3"

-> 201
{
  "id": "<voice_interaction_id>",
  "patient_id": "...",
  "device_id": "...",
  "transcript": "what the user asked, as heard by the AI",
  "answer_text": "the spoken answer, as text",
  "citations": [...],
  "tool_results": [...],
  "audio_format": "wav",
  "created_at": "..."
}
```

This call does the whole round trip server-side (transcribe → answer →
synthesize) and logs it, but the JSON body above does **not** include the
audio bytes themselves (kept out to keep the response small). Fetch the
actual playable audio with:

```
GET /devices/voice-query/{id}/audio
-> raw audio bytes, Content-Type: audio/wav or audio/mpeg
```
Play that back through your speaker/amp. Same file-not-stream advice
applies here on the way down: download the whole response then play it,
rather than trying to play while downloading.

## 6. Compartment & schedule semantics (recap for firmware logic)

- 7 compartments, letters `A`–`G`, fixed mapping — don't reassign meaning
  device-side, always key off the letter from the schedule payload.
- Each compartment holds whichever medications are meant to be taken
  together at that `dispense_time` — you dispense the whole compartment at
  once, you don't need to distinguish between the pills inside it.
- `frequency: "daily"` → fire every day at `dispense_time`.
  `frequency: "specific_days"` → only fire on the weekdays listed in
  `days_of_week` (lowercase 3-letter: `mon,tue,wed,thu,fri,sat,sun`).
  `frequency: "as_needed"` → don't auto-fire; only dispense that compartment
  on an explicit `manual_dispense` command.
- `start_date`/`end_date` (ISO dates, nullable) bound when a schedule is
  active if present; ignore compartments outside that window.
- If a `POST /caregivers/patients/{id}/schedules/sync` (or `sync` command)
  triggers, just replace your entire local schedule with the fresh
  `update_schedule` payload you're subsequently pushed/fetched.
- On boot before the WS handshake completes (or if it's been dropped for a
  while), you can pull the schedule directly instead of waiting on the push:
  `GET /devices/schedule` (same HTTPS auth headers as §2) returns the
  identical payload shape.
