from s3fs import S3FileSystem

from app.config import settings

s3 = S3FileSystem(
    key=settings.S3_ACCESS_KEY_ID,
    secret=settings.S3_SECRET_ACCESS_KEY,
    endpoint_url=settings.S3_ENDPOINT,
)
