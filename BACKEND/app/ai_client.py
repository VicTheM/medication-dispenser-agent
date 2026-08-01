"""
Thin wrapper around the already-deployed Ally Healthwise RAG API.

Only the endpoints this product actually needs are used:
  - GET  /health          -> surface AI service health to caregivers/ops
  - POST /ask             -> text Q&A (used by the web/app front end)
  - POST /voice/ask       -> full voice pipeline (audio in, transcript + spoken
                              answer out) - this is what the hardware uses
  - POST /ingest          -> rebuild the local knowledge index after a
                              caregiver uploads new material

/voice/ask (JSON+base64 audio back) rather than /voice/ask/file is used here
because our backend needs the transcript/citations metadata as well as the
audio, not just a raw audio download.
"""
import base64

import httpx

from app.config import settings


class AIClientError(RuntimeError):
    pass


async def check_health() -> dict:
    async with httpx.AsyncClient(base_url=settings.AI_API_BASE_URL, timeout=settings.AI_API_TIMEOUT_SECONDS) as client:
        resp = await client.get("/health")
        resp.raise_for_status()
        return resp.json()


async def ask_text(question: str) -> dict:
    async with httpx.AsyncClient(base_url=settings.AI_API_BASE_URL, timeout=settings.AI_API_TIMEOUT_SECONDS) as client:
        resp = await client.post("/ask", json={"question": question})
        if resp.status_code != 200:
            raise AIClientError(f"AI /ask failed: {resp.status_code} {resp.text}")
        return resp.json()


async def ask_voice(audio_bytes: bytes, filename: str, audio_format: str = "wav") -> dict:
    """
    Sends the recorded audio to the AI's /voice/ask endpoint and returns the
    parsed JSON: question, answer, citations, tool_results, transcript,
    audio_base64, audio_content_type, audio_format.
    """
    async with httpx.AsyncClient(base_url=settings.AI_API_BASE_URL, timeout=settings.AI_API_TIMEOUT_SECONDS) as client:
        files = {"audio": (filename, audio_bytes, "application/octet-stream")}
        data = {"audio_format": audio_format}
        resp = await client.post("/voice/ask", data=data, files=files)
        if resp.status_code != 200:
            raise AIClientError(f"AI /voice/ask failed: {resp.status_code} {resp.text}")
        return resp.json()


async def trigger_ingest() -> dict:
    async with httpx.AsyncClient(base_url=settings.AI_API_BASE_URL, timeout=settings.AI_API_TIMEOUT_SECONDS * 4) as client:
        resp = await client.post("/ingest")
        if resp.status_code != 200:
            raise AIClientError(f"AI /ingest failed: {resp.status_code} {resp.text}")
        return resp.json()


def decode_audio_base64(audio_base64: str) -> bytes:
    return base64.b64decode(audio_base64)
