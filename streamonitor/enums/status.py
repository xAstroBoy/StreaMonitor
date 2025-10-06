from enum import Enum


class Status(Enum):
    UNKNOWN = 1
    NOTRUNNING = 2
    ERROR = 3
    RESTRICTED = 1403
    PUBLIC = 200
    NOTEXIST = 400
    PRIVATE = 403
    OFFLINE = 404
    LONG_OFFLINE = 410
    DELETED = 444  # Model account has been deleted
    RATELIMIT = 429
    CLOUDFLARE = 503
