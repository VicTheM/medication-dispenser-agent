# MedAdhere — System Overview

*A reference document for writing a report on the system. Covers what the
system is, how its parts fit together, and how the hardware works.*

---

## 1. What the system does

MedAdhere is a medication scheduling and adherence platform built around a
piece of physical hardware: a 7-compartment automatic pill dispenser. A
**caregiver** (a family member or professional carer) enrols a **patient**,
hands them a dispenser, and configures what medication goes in which
compartment and when it should be dispensed. From that point on, the device
dispenses medication **autonomously** — on schedule, without anyone needing
to open an app — and reports back what happened: when it dispensed, a short
video confirming the patient actually took the dose, and ongoing device
health (battery, tray level, connectivity). Patients get a **read-only**
view of the same information. An **AI assistant**, grounded in
caregiver-supplied knowledge documents, can answer questions typed into the
app or asked out loud directly to the device.

The system has four actors:

| Actor | Role |
|---|---|
| **Caregiver** | Configures everything: enrols patients, assigns hardware, sets medications and schedules, monitors adherence |
| **Patient** | Views their own schedule, history, and device status — cannot change any configuration |
| **Hardware device** | Dispenses autonomously, confirms adherence on video, reports telemetry, hosts a voice-question interface |
| **AI assistant** | Answers medication/care questions via text or voice, grounded in documents the caregiver provides |

---

## 2. High-level architecture

```
 ┌─────────────┐        HTTPS (JSON)         ┌──────────────────┐
 │  Web app     │◄───────────────────────────►│                  │
 │ (caregiver + │                              │                  │
 │  patient UI) │                              │   MedAdhere      │
 └─────────────┘                               │   Backend        │
                                                │   (FastAPI)      │
 ┌─────────────┐   WebSocket (control plane)   │                  │
 │  Hardware    │◄──────────────────────────────►                 │
 │  dispenser   │                              │  - Auth (JWT)    │
 │              │   HTTPS (dispense events,    │  - SQLite DB     │
 │              │──────────────────────────────►  - Background    │
 │              │    adherence video,           │    jobs          │
 │              │    telemetry, voice audio)    │  - WS manager    │
 └─────────────┘                               └────────┬─────────┘
                                                          │ HTTPS
                                                          ▼
                                                ┌──────────────────┐
                                                │  AI service        │
                                                │ (Ally Healthwise,  │
                                                │  deployed          │
                                                │  separately)        │
                                                └──────────────────┘
```

Three independent pieces, each documented and delivered on its own:

1. **Backend API** (Python / FastAPI) — the source of truth. Owns the
   database, authenticates all three kinds of clients (caregiver, patient,
   device), and proxies to the AI service.
2. **Frontend web app** (React / TypeScript) — the landing page, caregiver
   dashboard, and patient portal. Talks to the backend over plain HTTPS/JSON.
3. **Hardware device** (physical unit, or the mock simulator used for
   testing) — talks to the backend two ways: a persistent WebSocket for
   real-time control, and plain HTTPS calls for reporting data.

The backend ships with **SQLite** — a single database file alongside the
application, no separate database server to run — which was a deliberate
choice to keep the whole backend a single deployable unit (also packaged
with a Dockerfile / docker-compose for one-command deployment).

---

## 3. The hardware

### 3.1 Physical description

The device is a **7-compartment dispenser**, compartments labeled `A`
through `G`. Each compartment is loaded with whichever medications a patient
needs to take together at one particular time of day — so a compartment
isn't "one pill," it's "everything due at 8am," bundled together. This
maps directly onto how the backend models a `Schedule`: one schedule row is
one compartment, holding a list of medications, a dispense time, and a
frequency (daily / specific weekdays / as-needed manual only).

Conceptually the device needs, per compartment, a small motor/gate mechanism
to release its contents, plus shared subsystems:

- **Wi-Fi radio** — connects to the backend; the reference implementation
  targets an ESP32-class microcontroller, chosen because it's a common,
  inexpensive, Wi-Fi-capable microcontroller family with enough compute for
  local scheduling logic, TLS, and a WebSocket client.
- **Camera** — records a short (~2 minute) clip immediately after each
  dispense, used as adherence confirmation ("did the patient actually take
  it," not just "did the mechanism fire").
- **Tray / fill sensor** — reports whether a compartment still has
  medication in it (`full` / `low` / `empty`).
- **Person-detection sensor** — (e.g. a simple presence sensor or inferred
  from the camera) reported alongside telemetry, to help distinguish "no one
  was there" from "device malfunctioned."
- **Motor/mechanism status sensor** — per-dispense health check, reported
  as part of telemetry (`motor_status`, `sensor_status`).
- **Microphone + speaker** — for the voice-question feature: the patient
  asks something out loud, the device records it, uploads it, and plays back
  a spoken answer.
- **Battery + charging circuit** — battery level is reported in telemetry
  and surfaced to caregivers as a low-battery alert.

### 3.2 How the device talks to the backend

Two channels, used for different purposes:

**WebSocket (`/devices/ws?device_uid=...&secret=...`)** — a single
persistent connection, opened right after Wi-Fi connects and authenticated
with credentials issued when the caregiver assigns the device to a patient.
Used purely as a **control plane**:
- On connect, the backend immediately pushes the full current schedule (all
  7 compartments) so the device can act on it without needing to ask.
- Whenever a caregiver edits a schedule, the backend pushes the updated
  schedule to the connected device automatically, in real time.
- The backend can push commands at any time: `manual_dispense` (trigger a
  specific compartment right now), `restart`, `sync` (ask the device to
  flush anything it cached while offline), and `configure`.
- The device acknowledges commands it receives (`{"type":"ack", ...}`) so
  the backend can track delivery.

**HTTPS endpoints** — used as the **data plane**, for anything that needs
guaranteed, individually-retriable delivery (especially the ~2-minute video,
which is a poor fit for a single control-channel stream):
- `POST /devices/dispense-event` — reported immediately after each
  successful (or failed) dispense.
- `POST /devices/adherence-video` — the confirmation clip, tagged to the
  dispense event it belongs to.
- `POST /devices/telemetry` — periodic device-health snapshot.
- `POST /devices/sync-offline` — a batch endpoint: if the device loses
  connectivity, it keeps dispensing autonomously from its locally stored
  schedule and queues events/telemetry, then flushes the whole queue here
  once connectivity returns.
- `POST /devices/voice-query` — the recorded audio question; the backend
  forwards it to the AI service and returns transcript + spoken answer.

A key design decision: **the schedule lives on the device itself.**
Dispensing does not require a live connection — the device autonomously
checks its locally stored schedule against its own clock and dispenses
regardless of Wi-Fi status, which is essential for a medical device (a
Wi-Fi outage should never mean a missed dose). Connectivity is only needed
to *report* what happened and to *receive updates*.

### 3.3 Voice pipeline, and why file upload beats streaming

The voice-question feature intentionally uses a **complete file upload**,
not a live audio stream, for the question audio. Reasoning: the AI service's
voice endpoint only accepts a complete multipart file in the first place; a
spoken question is a few seconds of audio (trivial to buffer on a
microcontroller); and a single HTTPS POST has much simpler failure/retry
behavior on a flaky Wi-Fi connection than managing a live duplex audio
stream would. The response (transcript, answer text, and synthesized speech
audio) comes back the same way — downloaded as a complete file, then played.

---

## 4. Backend design

### 4.1 Data model

Core entities: `Caregiver`, `Patient`, `Device`, `Medication`, `Schedule`
(one row per compartment assignment), `DispenseEvent`, `AdherenceVideo`,
`Telemetry`, `DeviceCommand` (audit trail of control-plane commands and
their ack status), `VoiceInteraction`, `KnowledgeDocument`, `Notification`.

### 4.2 Authentication — three different trust boundaries

- **Caregivers and patients** authenticate with email/password and get a
  JWT bearer token. Patients' accounts are created by their caregiver during
  enrolment (a patient never self-registers). Every route that mutates data
  requires a caregiver token; patient tokens only unlock read endpoints —
  enforced server-side, not just hidden in the UI.
- **Devices** authenticate with a `device_uid` / `device_secret` pair,
  generated once when a caregiver assigns hardware to a patient, sent as
  headers (`X-Device-Id` / `X-Device-Secret`) on HTTPS calls and as query
  parameters on the WebSocket URL.

### 4.3 Background jobs (in-process scheduler)

Rather than requiring a separate worker/queue system, the backend runs two
periodic jobs in-process (via APScheduler) that turn raw data into
actionable alerts:
- **Missed-dose detection** (every 5 minutes) — compares each active
  schedule's dispense time against actual dispense events, with a grace
  window, and raises a notification if a dose is late.
- **Device health** (every minute) — flags a device transitioning to
  offline (no contact past a grace period), and low-battery alerts
  (re-notified at most every 12 hours, not on every check).

### 4.4 AI integration

The backend proxies a subset of an already-deployed AI service's endpoints:
- `POST /ask` — typed questions from the web app.
- `POST /voice/ask` — the full voice pipeline (transcribe → answer →
  synthesize) used by both the device and, optionally, the app.
- `POST /ingest` — triggered after a caregiver uploads a knowledge document
  (a care plan, discharge summary, medication guide), so the assistant's
  answers stay grounded in material the caregiver actually provided rather
  than generic knowledge.

---

## 5. Frontend

A single React/TypeScript app with three surfaces:
- **Landing page** — public marketing page.
- **Caregiver dashboard** — patient list, and per-patient tabs covering
  every admin action (medications, schedule — including a clickable
  visual A–G compartment editor — device assignment and commands, dispense
  logs, adherence videos, telemetry, voice history, notifications) plus a
  caregiver-level knowledge base upload page.
- **Patient portal** — a read-only mirror: today's schedule, history, and
  notifications, plus the same "ask the assistant" widget.

A recurring visual element (a horizontal 7-segment "compartment strip"
showing A–G) is used consistently across the marketing page, the schedule
editor, and both dashboards' "today" views, since it maps directly onto the
physical device.

---

## 6. Testing without physical hardware

Because a physical unit wasn't available during development, a **mock
device script** (`mock_device.py`) stands in for it: it opens the same
WebSocket, authenticates with the same device credentials, receives and
stores the schedule the same way, dispenses autonomously when the simulated
clock hits a scheduled time, uploads a placeholder adherence clip, sends
telemetry, and can simulate going offline (queuing events) and coming back
online (flushing them via the sync endpoint). It also accepts live commands
typed into the terminal, so the whole system — caregiver dashboard down to
"hardware" behavior — can be exercised without physical hardware. This was
used to verify, end-to-end, that: schedule pushes over the WebSocket work,
manual-dispense commands issued from the caregiver dashboard reach the
"device" and get executed, offline-cached events correctly sync and are
flagged as such, and autonomous schedule-triggered dispensing fires at the
correct time without any manual trigger.

---

## 7. Limitations / room for future work

- Adherence video files are stored on disk with metadata in the database,
  but there's no streaming/preview endpoint yet — the caregiver dashboard
  shows video metadata, not an inline player.
- The caregiver dashboard fetches data on load/tab-switch rather than
  subscribing to live updates; a dispense event won't appear until the page
  is refreshed or revisited.
- Notification delivery is limited to in-app records — there's no
  email/SMS/push integration yet, just the underlying data caregivers can
  poll.
- The knowledge-feeding flow assumes the backend and the AI service can
  share a document folder (or a synced copy of one); if they're deployed on
  separate hosts this needs an explicit sync step.
