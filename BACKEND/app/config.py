import os

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # --- Core ---
    APP_NAME: str = "MedAdhere API"
    SECRET_KEY: str = os.getenv("SECRET_KEY", "change-this-secret-in-production")
    ALGORITHM: str = "HS256"
    ACCESS_TOKEN_EXPIRE_MINUTES: int = 60 * 24  # 24h for caregivers/patients

    # --- Database (SQLite file, ships alongside the app - no external DB needed) ---
    DATABASE_URL: str = os.getenv("DATABASE_URL", "sqlite:///./medadhere.db")

    # --- Storage (local disk, mounted volume in prod) ---
    STORAGE_DIR: str = os.getenv("STORAGE_DIR", "./storage")
    VIDEO_DIR: str = os.getenv("VIDEO_DIR", "./storage/videos")
    KNOWLEDGE_DIR: str = os.getenv("KNOWLEDGE_DIR", "./storage/knowledge")
    VOICE_DIR: str = os.getenv("VOICE_DIR", "./storage/voice")

    # --- Ally Healthwise AI service (already deployed) ---
    AI_API_BASE_URL: str = os.getenv("AI_API_BASE_URL", "http://localhost:9000")
    AI_API_TIMEOUT_SECONDS: float = float(os.getenv("AI_API_TIMEOUT_SECONDS", "30"))
    # If the AI service's /ingest endpoint reads from a local PDF folder, point
    # KNOWLEDGE_DIR (above) at that same folder (or a synced copy) so caregiver
    # uploads become visible to it before /ingest is triggered.

    # --- Device / hardware ---
    NUM_COMPARTMENTS: int = 7  # A - G
    DEVICE_OFFLINE_AFTER_SECONDS: int = 120  # no heartbeat/telemetry -> mark offline

    # --- File Storage Bucket ---
    S3_ENDPOINT: str = os.getenv("S3_ENDPOINT", "")
    S3_ACCESS_KEY_ID: str = os.getenv("S3_ACCESS_KEY_ID", "")
    S3_SECRET_ACCESS_KEY: str = os.getenv("S3_SECRET_ACCESS_KEY", "")


    class Config:
        env_file = ".env"


settings = Settings()

for d in (settings.STORAGE_DIR, settings.VIDEO_DIR, settings.KNOWLEDGE_DIR, settings.VOICE_DIR):
    os.makedirs(d, exist_ok=True)
