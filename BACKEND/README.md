# MedAdhere API

FastAPI backend for a medication scheduling & adherence system with four
actors: **caregivers**, **patients**, the **7-compartment hardware
dispenser**, and the **Ally Healthwise AI** assistant.

Ships with SQLite (a single `medadhere.db` file) so there's no separate
database server to stand up — good enough for launch, and swappable for
Postgres later by changing one env var.

## Run it

### Locally
```bash
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env   # edit SECRET_KEY and AI_API_BASE_URL at minimum
uvicorn app.main:app --reload
```

### With Docker (one command, includes a persistent volume for the SQLite file + uploaded media)
```bash
SECRET_KEY=$(openssl rand -hex 32) AI_API_BASE_URL=https://your-ai-service docker compose up --build
```

### Deploying to Render (or any host with an ephemeral filesystem)
Local disk gets wiped on every deploy/restart unless you attach persistent
storage. See `docs/DEPLOYMENT.md` for the two fixes (a mounted persistent
disk via the included `render.yaml`, or the recommended Postgres + object
storage path for production).

API docs: http://localhost:8000/docs

## What's in the box

- `app/models.py` — SQLAlchemy models for every entity (caregivers, patients,
  devices, medications, schedules, dispense events, adherence videos,
  telemetry, device commands, voice interactions, knowledge documents,
  notifications).
- `app/routers/auth.py` — caregiver registration/login, patient login.
- `app/routers/caregivers.py` — patient enrolment, hardware assignment,
  medication CRUD, schedule (compartment) CRUD with auto-sync to the device,
  device commands, and read access to logs/videos/telemetry/voice history.
- `app/routers/patients.py` — read-only mirror of a patient's own data.
- `app/routers/devices.py` — the hardware-facing surface: the WebSocket
  control channel plus HTTPS endpoints for dispense events, adherence video
  upload, telemetry, offline-cache sync, and voice queries.
- `app/routers/ai.py` — proxies the deployed Ally Healthwise AI (`/ask`,
  voice pipeline, `/ingest`) and lets caregivers feed it local knowledge docs.
- `app/ws_manager.py` — in-memory registry of live device WebSocket
  connections, used to push real-time commands.
- `app/ai_client.py` — thin httpx wrapper around the AI service.
- `app/scheduler.py` — in-process background jobs (APScheduler, no separate
  worker needed) that turn raw data into `Notification` rows: missed doses
  (checked every 5 min, with a 30-min grace period and same-day de-dupe),
  device-gone-offline transitions, and low-battery alerts (checked every
  1 min, at most once per 12h per device).

Two docs for your other teams:
- `docs/FRONTEND_GUIDE.md` — everything the web/mobile app developer needs.
- `docs/FIRMWARE_GUIDE.md` — everything the ESP32 firmware developer needs.

## Notes on the AI integration

The attached Ally Healthwise OpenAPI spec was used to identify which
endpoints this product actually needs:

- `POST /voice/ask` — used by both the hardware (`POST /devices/voice-query`)
  and, if the app also records audio, `POST /ai/voice-ask`. This is the
  endpoint that returns transcript + citations + tool_results + spoken-answer
  audio in one JSON response, which is what lets us log the interaction.
- `POST /ask` — used for typed questions from the web/mobile app
  (`POST /ai/ask`).
- `POST /ingest` — triggered after a caregiver uploads local knowledge via
  `POST /ai/knowledge`. Important: the AI service's `/ingest` rebuilds its
  index from a PDF folder it already has configured server-side — it does not
  accept file bytes in the request body. So `KNOWLEDGE_DIR` in this app must
  point at the same folder (or a synced copy of it) that the AI service
  reads from. If they run on different hosts, add a small sync step (rsync,
  shared volume, or object storage) before the `/ingest` call.
- `/speech/transcribe` and `/speech/synthesize` were **not** wired in
  separately, since `/voice/ask` already does transcribe → answer → synthesize
  in one round trip, which is all this product needs.
