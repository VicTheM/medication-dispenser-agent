import os
import uuid
from typing import List

from fastapi import APIRouter, Depends, File, HTTPException, UploadFile
from sqlalchemy.orm import Session

from app import ai_client, models, schemas
from app.config import settings
from app.database import get_db
from app.deps import get_current_caregiver, get_current_caregiver_or_patient
from app.bucket import s3

router = APIRouter(prefix="/ai", tags=["AI"])


@router.get("/health")
async def ai_health(_=Depends(get_current_caregiver_or_patient)):
    try:
        return await ai_client.check_health()
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"AI service unreachable: {exc}")


@router.post("/ask")
async def ask_question(payload: schemas.AskRequest, identity=Depends(get_current_caregiver_or_patient)):
    """Text Q&A for the web/mobile app (caregiver or patient)."""
    try:
        return await ai_client.ask_text(payload.question)
    except ai_client.AIClientError as exc:
        raise HTTPException(status_code=502, detail=str(exc))


@router.post("/voice-ask")
async def ask_voice_from_app(
    audio: UploadFile = File(...),
    audio_format: str = "wav",
    identity=Depends(get_current_caregiver_or_patient),
):
    """
    Same voice pipeline as the hardware uses, exposed here so the web/mobile
    app can also let a caregiver or patient ask a spoken question (e.g. typed
    text is fine too via /ai/ask - this is only needed if the app records audio).
    """
    audio_bytes = await audio.read()
    try:
        return await ai_client.ask_voice(audio_bytes, audio.filename or "query.wav", audio_format)
    except ai_client.AIClientError as exc:
        raise HTTPException(status_code=502, detail=str(exc))


# ---------------------------------------------------------------------------
# Caregiver: feed the AI local knowledge (uploads + trigger re-ingest)
# ---------------------------------------------------------------------------

@router.post("/knowledge", response_model=schemas.KnowledgeDocumentOut, status_code=201)
async def upload_knowledge_document(
    file: UploadFile = File(...),
    caregiver: models.Caregiver = Depends(get_current_caregiver),
    db: Session = Depends(get_db),
):
    """
    Uploads a document (e.g. a care plan, discharge summary, or medication
    guide PDF) for the AI to learn from.

    NOTE: the deployed Ally Healthwise service's POST /ingest endpoint rebuilds
    its vector index from a PDF folder it already has configured server-side -
    it doesn't accept file content in the request. So KNOWLEDGE_DIR here MUST
    be the same folder (or mounted/synced to the same location) that AI
    service reads from. If AI_API runs on a different host, point KNOWLEDGE_DIR
    at a shared volume, or add a small file-sync step here before calling
    trigger_ingest().
    """
    if not file.filename.lower().endswith((".pdf", ".txt", ".md")):
        raise HTTPException(status_code=422, detail="Only .pdf, .txt, or .md knowledge files are supported")

    filename = f"{uuid.uuid4().hex}_{file.filename}"
    dest_path = os.path.join(settings.KNOWLEDGE_DIR, filename)
    with s3.open(dest_path, "wb") as f:
        while chunk := await file.read(1024 * 1024):
            f.write(chunk)

    doc = models.KnowledgeDocument(caregiver_id=caregiver.id, filename=file.filename, file_path=dest_path)
    db.add(doc)
    db.commit()
    db.refresh(doc)

    try:
        ingest_result = await ai_client.trigger_ingest()
        doc.ingest_status = "ingested"
        doc.ingest_response = ingest_result
    except ai_client.AIClientError as exc:
        doc.ingest_status = "failed"
        doc.ingest_response = {"error": str(exc)}
    db.commit()
    db.refresh(doc)
    return doc


@router.get("/knowledge", response_model=List[schemas.KnowledgeDocumentOut])
def list_knowledge_documents(caregiver: models.Caregiver = Depends(get_current_caregiver),
                              db: Session = Depends(get_db)):
    return db.query(models.KnowledgeDocument).filter(models.KnowledgeDocument.caregiver_id == caregiver.id).all()


@router.post("/knowledge/reingest", status_code=202)
async def reingest_knowledge(caregiver: models.Caregiver = Depends(get_current_caregiver)):
    """Manually re-trigger the AI's index rebuild (e.g. after uploading several docs)."""
    try:
        return await ai_client.trigger_ingest()
    except ai_client.AIClientError as exc:
        raise HTTPException(status_code=502, detail=str(exc))
