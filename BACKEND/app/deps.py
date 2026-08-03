from typing import Optional

from fastapi import Depends, Header, HTTPException, status
from fastapi.security import OAuth2PasswordBearer
from sqlalchemy.orm import Session

from app import models
from app.database import get_db
from app.security import decode_access_token

oauth2_scheme = OAuth2PasswordBearer(tokenUrl="/auth/token", auto_error=False)


def _unauthorized(detail: str = "Could not validate credentials"):
    return HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail=detail,
                          headers={"WWW-Authenticate": "Bearer"})


def get_current_caregiver(
    token: Optional[str] = Depends(oauth2_scheme),
    db: Session = Depends(get_db),
) -> models.Caregiver:
    if not token:
        raise _unauthorized("Missing bearer token")
    payload = decode_access_token(token)
    if not payload or payload.get("role") != "caregiver":
        raise _unauthorized()
    caregiver = db.query(models.Caregiver).filter(models.Caregiver.id == payload["sub"]).first()
    if not caregiver:
        raise _unauthorized("Caregiver not found")
    return caregiver


def get_current_patient(
    token: Optional[str] = Depends(oauth2_scheme),
    db: Session = Depends(get_db),
) -> models.Patient:
    if not token:
        raise _unauthorized("Missing bearer token")
    payload = decode_access_token(token)
    if not payload or payload.get("role") != "patient":
        raise _unauthorized()
    patient = db.query(models.Patient).filter(models.Patient.id == payload["sub"]).first()
    if not patient:
        raise _unauthorized("Patient not found")
    return patient


def get_current_caregiver_or_patient(
    token: Optional[str] = Depends(oauth2_scheme),
    db: Session = Depends(get_db),
):
    if not token:
        raise _unauthorized("Missing bearer token")
    payload = decode_access_token(token)
    if not payload:
        raise _unauthorized()
    if payload.get("role") == "caregiver":
        obj = db.query(models.Caregiver).filter(models.Caregiver.id == payload["sub"]).first()
    elif payload.get("role") == "patient":
        obj = db.query(models.Patient).filter(models.Patient.id == payload["sub"]).first()
    else:
        raise _unauthorized()
    if not obj:
        raise _unauthorized()
    return payload["role"], obj


def require_owned_patient(patient_id: str, caregiver: models.Caregiver, db: Session) -> models.Patient:
    """Fetch a patient and 404/403 if it doesn't belong to this caregiver."""
    patient = db.query(models.Patient).filter(models.Patient.id == patient_id).first()
    if not patient:
        raise HTTPException(status_code=404, detail="Patient not found")
    if patient.caregiver_id != caregiver.id:
        raise HTTPException(status_code=403, detail="Not your patient")
    return patient


def authenticate_device(
    x_device_id: str = Header(..., alias="X-Device-Id"),
    x_device_secret: str = Header(..., alias="X-Device-Secret"),
    db: Session = Depends(get_db),
) -> models.Device:
    device = db.query(models.Device).filter(models.Device.device_uid == x_device_id).first()
    if not device or device.device_secret != x_device_secret:
        raise HTTPException(status_code=401, detail="Invalid device credentials")
    return device
