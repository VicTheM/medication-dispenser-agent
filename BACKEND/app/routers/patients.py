from typing import List

from fastapi import APIRouter, Depends
from sqlalchemy.orm import Session

from app import models, schemas
from app.database import get_db
from app.deps import get_current_patient
from app.schedule_utils import schedule_to_out

router = APIRouter(prefix="/patients", tags=["Patients (read-only)"])


@router.get("/me", response_model=schemas.PatientOut)
def get_me(patient: models.Patient = Depends(get_current_patient)):
    return patient


@router.get("/me/medications", response_model=List[schemas.MedicationOut])
def my_medications(patient: models.Patient = Depends(get_current_patient), db: Session = Depends(get_db)):
    return db.query(models.Medication).filter(models.Medication.patient_id == patient.id).all()


@router.get("/me/schedules", response_model=List[schemas.ScheduleOut])
def my_schedules(patient: models.Patient = Depends(get_current_patient), db: Session = Depends(get_db)):
    schedules = db.query(models.Schedule).filter(models.Schedule.patient_id == patient.id).all()
    return [schedule_to_out(s) for s in schedules]


@router.get("/me/dispense-logs", response_model=List[schemas.DispenseEventOut])
def my_dispense_logs(limit: int = 100, patient: models.Patient = Depends(get_current_patient),
                      db: Session = Depends(get_db)):
    events = (
        db.query(models.DispenseEvent)
        .filter(models.DispenseEvent.patient_id == patient.id)
        .order_by(models.DispenseEvent.dispensed_at.desc())
        .limit(limit)
        .all()
    )
    out = []
    for e in events:
        item = schemas.DispenseEventOut.model_validate(e)
        item.has_video = e.adherence_video is not None
        out.append(item)
    return out


@router.get("/me/adherence-videos", response_model=List[schemas.AdherenceVideoOut])
def my_adherence_videos(patient: models.Patient = Depends(get_current_patient), db: Session = Depends(get_db)):
    return (
        db.query(models.AdherenceVideo)
        .join(models.DispenseEvent)
        .filter(models.DispenseEvent.patient_id == patient.id)
        .order_by(models.AdherenceVideo.uploaded_at.desc())
        .all()
    )


@router.get("/me/device", response_model=schemas.DeviceStatusOut)
def my_device(patient: models.Patient = Depends(get_current_patient), db: Session = Depends(get_db)):
    from fastapi import HTTPException
    if not patient.device:
        raise HTTPException(status_code=404, detail="No device assigned yet")
    return patient.device


@router.get("/me/voice-interactions", response_model=List[schemas.VoiceInteractionOut])
def my_voice_interactions(patient: models.Patient = Depends(get_current_patient), db: Session = Depends(get_db)):
    return (
        db.query(models.VoiceInteraction)
        .filter(models.VoiceInteraction.patient_id == patient.id)
        .order_by(models.VoiceInteraction.created_at.desc())
        .all()
    )


@router.get("/me/notifications", response_model=List[schemas.NotificationOut])
def my_notifications(patient: models.Patient = Depends(get_current_patient), db: Session = Depends(get_db)):
    return (
        db.query(models.Notification)
        .filter(models.Notification.patient_id == patient.id)
        .order_by(models.Notification.created_at.desc())
        .all()
    )
