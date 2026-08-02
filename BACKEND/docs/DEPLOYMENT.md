# Deployment Notes — Render (and "why did my data disappear?")

## The problem

Render's web services run on an **ephemeral filesystem**. Anything written
to local disk — the SQLite database file, uploaded adherence videos,
knowledge documents, voice recordings — is wiped every time Render recycles
the container: on every deploy, every manual restart, and (on the free tier)
every time the instance spins down after inactivity and a new request spins
it back up. This is expected Render behavior, not something specific to
this app, and it affects **any** app that writes to local disk on Render
without extra configuration.

There are two ways to fix it, depending on your plan and how far you want
to take it.

---

## Option A — Quick fix: attach a persistent disk

**Requires a paid Render instance type** (persistent disks aren't available
on the free tier).

1. In the Render dashboard, open your service → **Disks** → **Add Disk**.
   - Mount path: `/app/data`
   - Size: 5GB is plenty to start (SQLite + a modest number of videos)
2. Set these environment variables so the app writes *inside* that mounted
   path instead of the container's regular (ephemeral) disk:
   ```
   DATABASE_URL=sqlite:////app/data/medadhere.db
   STORAGE_DIR=/app/data/storage
   VIDEO_DIR=/app/data/storage/videos
   KNOWLEDGE_DIR=/app/data/storage/knowledge
   VOICE_DIR=/app/data/storage/voice
   ```
3. Redeploy. From now on, that disk survives deploys and restarts.

Or skip the manual dashboard steps entirely and use the included
`render.yaml` blueprint (New → Blueprint in Render, point it at this repo) —
it provisions the service with the disk and env vars already set.

**Limitation to know about:** a Render disk is attached to a single
instance. If you ever scale to more than one instance (for load, not just
zero-downtime deploys), each instance gets its own disk and they won't share
data — you'd see patients/schedules depending on which instance answered
the request. Fine for a single-instance deployment, not fine long-term.

---

## Option B — Recommended for production: external Postgres + object storage

This removes the dependency on any instance's local disk entirely, so it
also solves the multi-instance problem Option A has.

### Database: swap SQLite for Postgres

1. Create a Postgres database (Render's own managed Postgres, or any
   provider — Supabase, Neon, RDS, etc.).
2. Add the driver:
   ```
   pip install psycopg2-binary
   # or add `psycopg2-binary>=2.9` to requirements.txt
   ```
3. Set `DATABASE_URL` to the Postgres connection string Render (or your
   provider) gives you, e.g.:
   ```
   DATABASE_URL=postgresql://user:password@host:5432/medadhere
   ```
   No other code changes needed — `app/database.py` already only applies
   SQLite-specific connect args conditionally, so this is a drop-in swap.
4. Redeploy. `Base.metadata.create_all()` in `app/main.py` will create the
   schema on first boot against Postgres the same way it does for SQLite.

### File storage: move off local disk

Videos, knowledge documents, and voice recordings still need somewhere
durable to live. Point the app at an S3-compatible bucket (AWS S3,
Cloudflare R2, Backblaze B2 all work the same way) instead of local disk:

- Simplest approach: mount the bucket at the filesystem paths the app
  already uses (`STORAGE_DIR` etc.) with something like `s3fs` or `rclone
  mount`, so no application code changes are needed.
- More robust approach (a bit more work, not currently implemented here):
  swap the `open(...)`/`write()` calls in `app/routers/devices.py` and
  `app/routers/ai.py` for an S3 client (`boto3`) upload, and store the
  resulting object URL/key in the database instead of a local file path.

Either way, once files aren't on the container's local disk, restarts and
redeploys stop being a data-loss event.

---

## Which should you pick?

- **Testing / a single caregiver-facing demo**: Option A (persistent disk)
  is fine and much less setup.
- **Anything real, multiple users, or you plan to scale**: Option B
  (Postgres + object storage) is the one to actually build toward — it's
  also what you'd want regardless of Render specifically, since local disk
  on any container platform (Render, Fly.io, Railway, ECS, Cloud Run...)
  behaves the same way unless you explicitly attach persistent storage.
