from s3fs import S3FileSystem

from app.config import settings

s3 = S3FileSystem(
    key=settings.R2_ACCESS_KEY_ID,
    secret=settings.R2_SECRET_ACCESS_KEY,
    endpoint_url=settings.R2_ENDPOINT,
)
