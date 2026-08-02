FROM python:3.12-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends gcc && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

# SQLite DB file and uploaded media live here - mount a volume onto /app/data in production
ENV DATABASE_URL=sqlite:////app/data/medadhere.db \
    STORAGE_DIR=/app/data/storage \
    VIDEO_DIR=/app/data/storage/videos \
    KNOWLEDGE_DIR=/app/data/storage/knowledge \
    VOICE_DIR=/app/data/storage/voice

RUN mkdir -p /app/data/storage/videos /app/data/storage/knowledge /app/data/storage/voice

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
