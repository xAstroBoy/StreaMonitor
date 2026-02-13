from enum import Enum


class Status(Enum):
    UNKNOWN = 1
    NOTRUNNING = 2
    ERROR = 3
    RESTRICTED = 1403
    ONLINE = 100
    PUBLIC = 200
    NOTEXIST = 400
    PRIVATE = 403
    OFFLINE = 404
    DELETED = 444 
    RATELIMIT = 429
    CLOUDFLARE = 503
