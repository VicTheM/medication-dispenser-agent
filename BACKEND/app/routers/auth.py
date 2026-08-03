from fastapi import APIRouter, Depends, HTTPException
from fastapi.security import OAuth2PasswordRequestForm
from sqlalchemy.orm import Session

from app import models, schemas
from app.database import get_db
from app.security import create_access_token, hash_password, verify_password

router = APIRouter(prefix="/auth", tags=["Auth"])


@router.post("/caregiver/register", response_model=schemas.CaregiverOut, status_code=201)
def register_caregiver(payload: schemas.CaregiverCreate, db: Session = Depends(get_db)):
    if db.query(models.Caregiver).filter(models.Caregiver.email == payload.email).first():
        raise HTTPException(status_code=409, detail="Email already registered")
    caregiver = models.Caregiver(
        full_name=payload.full_name,
        email=payload.email,
        phone=payload.phone,
        hashed_password=hash_password(payload.password),
    )
    db.add(caregiver)
    db.commit()
    db.refresh(caregiver)
    return caregiver


@router.post("/caregiver/login", response_model=schemas.Token)
def login_caregiver(payload: schemas.LoginRequest, db: Session = Depends(get_db)):
    caregiver = db.query(models.Caregiver).filter(models.Caregiver.email == payload.email).first()
    if not caregiver or not verify_password(payload.password, caregiver.hashed_password):
        raise HTTPException(status_code=401, detail="Invalid email or password")
    token = create_access_token(subject=caregiver.id, role="caregiver")
    return schemas.Token(access_token=token, role="caregiver")


@router.post("/patient/login", response_model=schemas.Token)
def login_patient(payload: schemas.LoginRequest, db: Session = Depends(get_db)):
    patient = db.query(models.Patient).filter(models.Patient.email == payload.email).first()
    if not patient or not patient.hashed_password or not verify_password(payload.password, patient.hashed_password):
        raise HTTPException(status_code=401, detail="Invalid email or password")
    token = create_access_token(subject=patient.id, role="patient")
    return schemas.Token(access_token=token, role="patient")


@router.post("/token", response_model=schemas.Token, include_in_schema=False)
def login_for_swagger(form_data: OAuth2PasswordRequestForm = Depends(), db: Session = Depends(get_db)):
    """Form-encoded login used by Swagger UI's Authorize button (username=email)."""
    caregiver = db.query(models.Caregiver).filter(models.Caregiver.email == form_data.username).first()
    if caregiver and verify_password(form_data.password, caregiver.hashed_password):
        token = create_access_token(subject=caregiver.id, role="caregiver")
        return schemas.Token(access_token=token, role="caregiver")

    patient = db.query(models.Patient).filter(models.Patient.email == form_data.username).first()
    if patient and patient.hashed_password and verify_password(form_data.password, patient.hashed_password):
        token = create_access_token(subject=patient.id, role="patient")
        return schemas.Token(access_token=token, role="patient")

    raise HTTPException(status_code=401, detail="Invalid email or password")
