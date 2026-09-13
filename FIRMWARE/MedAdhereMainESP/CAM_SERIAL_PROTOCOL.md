# CAM Board Serial Link

**Architecture change from the original plan**: the CAM/audio board is now a
second ESP32-S3 with its own WiFi connection, and it talks to the backend
(`/devices/voice-query`, `/devices/adherence-video`) **directly** — it no
longer sends file bytes back through the main board. The main board's job
shrank to: (1) hand the CAM board its WiFi/device credentials once at boot,
and (2) trigger a task and wait for pass/fail.

Wiring: MainESP GPIO17(RX)←CAM TX, GPIO18(TX)→CAM RX, common GND.
**Baud: 115200**, `SERIAL_8N1` (no longer bandwidth-critical, since no file
bytes cross this link anymore).

## 1. Config handoff (once, right at boot)

The instant the main board boots, before it even connects to WiFi itself, it
sends its NVS config to the CAM board as plain text lines:

```
CFGSTART
WIFI_SSID=<ssid>
WIFI_PASS=<password>
API_BASE=<https://your-backend>
DEVICE_UID=<device_uid>
DEVICE_SECRET=<device_secret>
UTC_OFFSET=<integer hours, e.g. 1 for GMT+1>
CFGEND
```

The CAM board should parse these key=value lines between the markers, save
them (its own NVS is fine), and use them to connect to WiFi and authenticate
to the backend using the **same** `X-Device-Id` / `X-Device-Secret` headers
the main board uses — the backend treats both boards as one logical device.

Re-sent on every main-board boot, so if the caregiver changes WiFi/device
credentials via the config portal (which restarts the main board), the CAM
board gets the update automatically on the next boot too.

## 2. Task trigger + result

```
Main -> CAM : single byte 'a'  (do the audio/voice task)
Main -> CAM : single byte 'v'  (do the video/adherence task)
CAM  -> Main: single byte 0    (success)
CAM  -> Main: single byte 1    (error)
```

That's the entire runtime protocol. No framing, no length prefix, no
checksum — the CAM board does the whole task (record, call the AI service
or upload the adherence capture, play back audio if applicable) on its own
and just reports back pass/fail.

Main board timeouts if no result arrives: `AUDIO_TIMEOUT_MS` (20s) for `'a'`,
`VIDEO_TIMEOUT_MS` (150s) for `'v'` — both in `config.h`. A timeout is
treated the same as receiving a `1`.

Any byte other than `0`/`1` seen while waiting is ignored (in case some
debug print leaks onto the shared line) rather than treated as a result.

## 3. What "success" means for each trigger

- **`'a'` (audio)**: CAM board should record the question, POST it to
  `/devices/voice-query`, get back the transcript + answer + a spoken-answer
  audio reference, fetch that audio, and play it through its speaker. Only
  report `0` once playback is actually done (or started reliably) — `1` for
  any failure along that chain (recording, network, playback).
- **`'v'` (video/adherence)**: CAM board should capture whatever adherence
  evidence it's designed to capture and POST it to
  `/devices/adherence-video` (needs a `dispense_event_id` - see note below),
  reporting `0` on a successful upload, `1` otherwise.

**Open item**: the CAM board needs to know which `dispense_event_id` to
attach the adherence upload to. Since that's created by the main board when
it reports the dispense event, either (a) extend the `'v'` trigger to
include that ID as extra bytes after the letter (simple line of text,
newline-terminated, mirroring the config handoff style), or (b) have the
backend accept the adherence video without an ID and match it up by
timestamp/device server-side. Not implemented yet — pick one before wiring
this up for real.
