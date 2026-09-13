import s3fs

from app.config import settings

s3 = s3fs.S3FileSystem(
    key=settings.S3_ACCESS_KEY_ID,
    secret=settings.S3_SECRET_ACCESS_KEY,
    endpoint_url=settings.S3_ENDPOINT,
)
