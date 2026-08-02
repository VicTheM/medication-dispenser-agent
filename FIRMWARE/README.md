# MedAdhere Mock Device

A terminal tool that simulates a real dispenser talking to the MedAdhere
backend, so you can exercise the whole app (caregiver dashboard, patient
portal, notifications) without physical hardware.

Tested end-to-end against a running backend: WebSocket connect + schedule
push, manual dispense via server command, autonomous schedule-triggered
dispense, adherence video upload, telemetry, offline caching + resync on
reconnect, and the voice pipeline all confirmed working.

## Setup

```bash
pip install httpx websockets
```

## Get a device_uid / device_secret

From the caregiver dashboard: enrol a patient, go to their **Device** tab,
and assign a device with any serial you like (e.g. `DEV-001`) — the secret
is shown once. Or via the API directly:

```bash
curl -X POST http://localhost:8000/caregivers/patients/{patient_id}/device \
  -H "Authorization: Bearer <caregiver token>" \
  -H "Content-Type: application/json" \
  -d '{"device_uid": "DEV-001"}'
```

## Run it

```bash
python mock_device.py --device-uid dev-001 --device-secret 02cf42feb2bae9410fc827c10fff3212c8601cf1f2297267
```

Add `--api-base https://your-deployed-backend` if you're not testing
locally (defaults to `http://localhost:8000`).

## What it does automatically

- Fetches its schedule on startup and keeps it updated live over the
  WebSocket whenever a caregiver edits it.
- Dispenses on its own when the simulated clock hits a scheduled
  compartment's time (same behavior real firmware is expected to have),
  reports the event, and uploads a placeholder adherence "video."
- Sends periodic telemetry (battery, tray state, person-detected, etc.).
- Executes real-time commands pushed from the caregiver dashboard —
  manual dispense, restart, sync, configure — and acks them.

## Commands (type `help` in the tool for the full list)

| Command | Effect |
|---|---|
| `status` | connection/sensor/schedule summary |
| `schedule` | print the schedule currently stored on the device |
| `dispense C [status]` | simulate dispensing compartment C right now |
| `telemetry` | send one telemetry report immediately |
| `battery 42` / `tray low` / `person off` | change simulated sensor readings |
| `offline` / `online` | simulate losing/regaining connectivity — events queue locally and flush via `/devices/sync-offline` on reconnect |
| `restart` | simulate a reboot |
| `voice demo` | send a tiny silent WAV through the AI voice pipeline (needs `AI_API_BASE_URL` configured on the backend) |
| `voice /path/to/file.wav` | send a real recorded question instead |
| `autodispense on\|off` | toggle autonomous scheduled dispensing |
| `quit` | stop |

## A typical test session

1. Start the backend, enrol a patient, assign a device, add a medication,
   and schedule it for ~1 minute from now.
2. Run this tool with that device's credentials — leave it running and
   watch the caregiver dashboard's Overview tab; the compartment should
   dispense on its own at the scheduled time, and the dispense log /
   adherence video entry should appear without you touching anything.
3. Try `offline`, then `dispense B`, then `online` — confirm the dispense
   log shows `was_offline_cached: true` and the device status flips back
   to online.
4. Try sending a **manual dispense** command from the caregiver dashboard's
   Device tab — the tool should print it arriving over the WebSocket and
   act on it automatically.
