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

### The ESP32-CAM video/audio TCP servers + Railway (`railway/`)

The FastAPI app above only speaks HTTP/WebSocket. The ESP32-CAM firmware's
video and audio streams are raw TCP, not HTTP, so they're served by a
separate script, `video-audio-server.py` (also duplicated inside
`railway/`, which is what's actually deployed — keep the two in sync if you
edit one). It runs two independent protocols:

- **Video**: accepts a JSON header (`width`/`height`/`fps`/`format`) followed
  by raw RGB565 frames, and writes an MP4 to the configured S3-compatible
  bucket.
- **Audio**: accepts a JSON header + raw PCM, saves the recording to the
  bucket, forwards it to the Ally Healthwise AI's `/voice/ask` endpoint
  (`AI_VOICE_URL`), resamples the spoken-answer audio back to 16kHz mono
  PCM, and streams it back to the device over the same connection.

Both protocols used to run as two threads in one process, each on its own
fixed port (5001/5002). That doesn't work on Railway, which only exposes a
single dynamic port (`$PORT`) per service. The script now takes a
`SERVER_TYPE` env var (`video` or `audio`) and runs **only that one**
server, bound to `$PORT` — so each protocol is deployed as its own Railway
service, both built from the same script/image.

`railway/` holds the Railway-specific copy of that script plus:

- `railway/requirements.txt` — the slimmer dependency set the TCP servers
  need (`numpy`, `opencv-python-headless`, `httpx`, `s3fs`) — no FastAPI/DB
  stack.
- `railway/.railway/railway.ts` — Railway's "infrastructure as code" config
  (via the `railway` npm package). It declares two services from the same
  source, `video-server` and `audio-server`, each with `SERVER_TYPE` set
  accordingly, `tcpProxy: true` (so Railway gives it a public TCP
  host/port instead of HTTPS), and the shared env (S3/R2 credentials, the
  AI voice endpoint, and `API_BASE_URL` pointing back at this app's
  deployed URL).
- `railway/.railway/README.md` — Railway config CLI reference
  (`railway config init|pull|plan|apply`).
- `railway/package.json` / `package-lock.json` — pulls in the `railway` npm
  package the `.railway/railway.ts` file imports.

**Running a server locally:**
```bash
cd railway
pip install -r requirements.txt
SERVER_TYPE=video PORT=5001 python video-audio-server.py   # in one shell
SERVER_TYPE=audio PORT=5002 python video-audio-server.py   # in another
```
Point `hardware-emulator.py` (or real firmware) at whichever host/ports
you're running these on to exercise them without hardware.

**Deploying to Railway:**
```bash
cd railway
npm install                       # pulls in the `railway` package .railway/railway.ts imports
railway config plan                # preview what Railway would create/change (safe, no-op)
railway config apply --yes         # apply it (add --confirm-destructive if it reports destructive changes)
railway up --service video-server  # push code + redeploy that service
railway up --service audio-server  # ditto for the audio service
```
Each service gets its own public TCP host/port from Railway once
deployed (that's the target you point the firmware/emulator at in
production) — grab them from the Railway dashboard or `railway status`.

After a clip is saved to the bucket, the server registers its public URL
with the main FastAPI app's own `/uploads` endpoint — see below — so it
shows up alongside the rest of a patient's data.

## New: `/uploads`

`app/routers/uploads.py` adds a small `Uploads` table (`app/models.py`) that
the TCP video/audio servers write to after they finish saving a clip to the
bucket, via a plain `POST` back to this API:

- `POST /uploads/` — body `{"upload_type": "video"|"audio", "url": "<public bucket URL>"}`,
  returns the created row (`id`, `type`, `url`, `created_at`). Called by
  `create_upload_record()` in `hardware-emulator.py`/`video-audio-server.py`
  right after a recording/response clip is written to storage.
- `GET /uploads/` — lists every upload record.

This is how a video/audio clip written by the TCP servers (which have no
direct DB access) becomes visible to the rest of the system: they write the
file to the bucket, then hit this HTTP endpoint with the resulting URL.

## Storage config note

The S3/R2 bucket settings in `app/config.py` were renamed from `R2_*` to
`S3_*` (`S3_ENDPOINT`, `S3_ACCESS_KEY_ID`, `S3_SECRET_ACCESS_KEY`) to match
the env vars the `railway/` services use — update your `.env` if you're
upgrading from an older checkout.

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
- `app/routers/uploads.py` — records video/audio clips saved by the TCP
  servers in `railway/` (see below) so they're visible in the main DB.
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
