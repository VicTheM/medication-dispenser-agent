from fastapi import APIRouter, Depends, HTTPException, status
from sqlalchemy.orm import Session
from sqlalchemy.exc import SQLAlchemyError
from app.database import get_db

from app import schemas, models


router = APIRouter(prefix="/uploads", tags=["Uploads"])

# --- Routes ---


@router.post(
    "/", response_model=schemas.UploadResponse, status_code=status.HTTP_201_CREATED
)
def create_upload(payload: schemas.UploadCreate, db: Session = Depends(get_db)):
    try:
        # Map 'upload_type' from the JSON payload to the 'type' column in the database
        db_upload = models.Uploads(type=payload.upload_type, url=str(payload.url))
        db.add(db_upload)
        db.commit()
        db.refresh(db_upload)
        return db_upload

    except SQLAlchemyError as e:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="A database error occurred while saving the upload.",
        )
    except Exception as e:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"An unexpected error occurred: {str(e)}",
        )


@router.get(
    "/", response_model=list[schemas.UploadResponse], status_code=status.HTTP_200_OK
)
def get_all_uploads(db: Session = Depends(get_db)):
    try:
        uploads = db.query(models.Uploads).all()
        return uploads

    except Exception as e:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="A database error occurred while fetching uploads.",
        )
