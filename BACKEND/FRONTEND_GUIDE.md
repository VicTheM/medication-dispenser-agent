# Frontend Developer Guide — MedAdhere API

Base URL: [https://medication-dispenser-agent.onrender.com](https://medication-dispenser-agent.onrender.com) (local dev: `http://localhost:8000`)

Everything is JSON over HTTPS except file uploads (multipart) and one
WebSocket used only by the hardware — the app never touches the WebSocket.

## 1. Actors & what they can do

| Actor | Can do |
|---|---|
| **Caregiver** | Register/login, enrol patients, assign hardware, create/edit medications & schedules, send device commands, view all logs/videos/telemetry for their patients, feed the AI knowledge docs |
| **Patient** | Login (if a caregiver set them up with an email/password), **read-only** view of their own medications, schedules, dispense logs, adherence videos, device status, voice-interaction history, notifications |
| **Hardware** | Not part of the frontend surface — see `FIRMWARE_GUIDE.md` |

Patients can never write. There is no PATCH/POST/DELETE route a patient
token will pass — every mutating caregiver route requires a caregiver JWT.

## 2. Auth

All auth returns a JWT bearer token, used as `Authorization: Bearer <token>`.
Tokens expire after 24h — re-login when you get a 401.

```
POST /auth/caregiver/register   { full_name, email, phone?, password }
POST /auth/caregiver/login      { email, password } -> { access_token, role: "caregiver" }
POST /auth/patient/login        { email, password } -> { access_token, role: "patient" }
```

Patients don't self-register — a caregiver creates their login when
enrolling them (`POST /caregivers/patients`, `password` field is optional; if
omitted, the patient has no login and only the caregiver can see their data).

## 3. Caregiver flows

### Enrol a patient & hand them a device
```
POST /caregivers/patients
  { full_name, date_of_birth?, phone?, email?, password?, notes?, timezone? }
  -> PatientOut { id, ... }

POST /caregivers/patients/{patient_id}/device
  { device_uid }   # the serial/QR code printed on the physical unit
  -> DeviceOut { id, device_uid, device_secret, status, ... }
```
`device_secret` is shown once here — hand it to whoever provisions the unit
(see firmware guide). Store it if you need to display it again via
`GET /caregivers/patients/{id}/device` (note: that endpoint omits the secret
in its response model, `DeviceStatusOut` — only the assignment call returns
the full `DeviceOut` with the secret).

### Medications
```
POST   /caregivers/patients/{patient_id}/medications   { name, dosage?, form?, instructions?, prescribing_doctor? }
GET    /caregivers/patients/{patient_id}/medications
PATCH  /caregivers/medications/{medication_id}
DELETE /caregivers/medications/{medication_id}
```

### Schedules (compartment assignment)
The hardware has 7 compartments, A–G. One schedule row = one compartment:
what medications are bundled into it, what time it fires, and how often.
Creating/updating/deleting a schedule **automatically pushes the updated
7-compartment schedule to the device** over its live WebSocket connection (if
it's online — see the firmware guide for offline handling).

```
POST /caregivers/patients/{patient_id}/schedules
  {
    "compartment": "A",                # one letter A-G
    "medication_ids": ["<med_id>", ...],
    "dispense_time": "08:00",          # 24h HH:MM, patient's local timezone
    "frequency": "daily",              # daily | specific_days | as_needed
    "days_of_week": ["mon","wed","fri"],  # required only if specific_days
    "start_date": "2026-08-01",        # optional
    "end_date": null
  }
```
A compartment can only have one *active* schedule at a time (409 if you try
to double-book one) — deactivate or delete the old one first if reassigning.

```
GET    /caregivers/patients/{patient_id}/schedules
PATCH  /caregivers/schedules/{schedule_id}
DELETE /caregivers/schedules/{schedule_id}
POST   /caregivers/patients/{patient_id}/schedules/sync   # force re-push to device
```

### Device control
```
GET  /caregivers/patients/{patient_id}/device            # status: online/offline/unassigned, battery, last_seen...
POST /caregivers/devices/{device_uid}/commands
  { "command_type": "manual_dispense", "payload": {"compartment": "C"} }
  # command_type: update_schedule | manual_dispense | restart | sync | configure
```
Response includes `"delivered": true/false` — `false` means the device was
offline; the command isn't automatically retried, so re-send once you see
the device come back online (poll `GET /caregivers/patients/{id}/device`).

### Monitoring
```
GET /caregivers/patients/{patient_id}/dispense-logs        # ?limit=100
GET /caregivers/patients/{patient_id}/adherence-videos
GET /caregivers/patients/{patient_id}/telemetry            # ?limit=50, most recent first
GET /caregivers/patients/{patient_id}/voice-interactions
GET /caregivers/patients/{patient_id}/notifications
```
`dispense-logs` items include `has_video: bool` so you know whether to offer
a "watch adherence video" link. Video files themselves aren't streamed
through this API in this version — `adherence-videos` gives you the DB
record (`file_path`, `duration_seconds`, `person_detected`, `uploaded_at`);
serve/stream the file from wherever `STORAGE_DIR` is mounted (e.g. behind a
CDN or a small static file route) rather than through the JSON API.

## 4. Patient flows (read-only mirror)

```
GET /patients/me
GET /patients/me/medications
GET /patients/me/schedules
GET /patients/me/dispense-logs
GET /patients/me/adherence-videos
GET /patients/me/device
GET /patients/me/voice-interactions
GET /patients/me/notifications
```
Same shapes as the caregiver equivalents, scoped to the logged-in patient.

## 5. AI assistant (for the app itself, not just the hardware)

```
POST /ai/ask          { "question": "..." }   -> question, answer, citations, tool_results
POST /ai/voice-ask     multipart: audio file, audio_format (wav|mp3)
GET  /ai/health                                -> AI service health/config flags
```
Both require a caregiver or patient bearer token. Use `/ai/ask` for typed
questions in the app; `/ai/voice-ask` only if your app itself records audio
(the primary voice channel is the hardware, documented in the firmware
guide).

## 6. Feeding the AI local knowledge (caregiver only)

```
POST /ai/knowledge          multipart: file (.pdf/.txt/.md)  -> KnowledgeDocumentOut
GET  /ai/knowledge                                            -> list of uploaded docs + ingest_status
POST /ai/knowledge/reingest                                    -> manually re-trigger index rebuild
```
Upload returns immediately with `ingest_status: "ingested"` or `"failed"` —
check that field (and `GET /ai/knowledge` afterwards) to know if the AI
actually picked up the new material; a `"failed"` status usually means the
AI service's `/ingest` call errored (see its own health/config).

## 7. Error shape

Standard FastAPI errors: `{"detail": "..."}` with the relevant HTTP status
(401 unauthenticated, 403 not yours, 404 not found, 409 conflict, 422
validation, 502 upstream AI service unreachable).

## 8. Things intentionally left out of this API (build in the app, not here)

- Push notifications delivery — `notifications` rows are created server-side
  (missed dose, device offline, schedule updated); wire your own
  push/email/SMS delivery on top by polling or adding a webhook later.
- Rendering/streaming video and audio files — the API returns file paths /
  base64 blobs; serve them via your CDN/static layer.


