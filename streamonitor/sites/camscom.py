import requests
from dataclasses import dataclass
from typing import Optional, Tuple, List

from streamonitor.bot import Bot
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ── CamsCom expected keys & known online codes ──────────────────────────
_CC_EXPECTED_KEYS: dict = {
    "": frozenset({"stream_name", "online"}),
}
_CC_PUBLIC_CODES: frozenset = frozenset({1, 2, 6, 10, 11, 12})
_CC_PRIVATE_CODES: frozenset = frozenset({3, 4, 7, 13, 14})
_CC_KNOWN_ONLINE_CODES: frozenset = frozenset(
    {str(c) for c in _CC_PUBLIC_CODES | _CC_PRIVATE_CODES | {0}}
)


@dataclass(frozen=True, slots=True)
class CCModelInfo:
    """Typed reader for the CamsCom ``/models/stream/`` response."""

    stream_name: str
    online: int  # normalised to int; -1 = missing/unparseable

    @classmethod
    def from_response(cls, data: dict, username: str = "", logger=None) -> "CCModelInfo":
        check_unknown_fields(data, _CC_EXPECTED_KEYS, "CC", username, logger)
        raw_online = data.get("online")
        try:
            online = int(raw_online) if raw_online is not None else -1
        except (ValueError, TypeError):
            online = -1
        return cls(
            stream_name=data.get("stream_name", "") or "",
            online=online,
        )

    def to_bot_status(self, username: str = "", logger=None) -> "Status":
        if not self.stream_name:
            return Status.NOTEXIST
        if self.online == 0:
            return Status.OFFLINE
        if self.online in _CC_PUBLIC_CODES:
            return Status.PUBLIC
        if self.online in _CC_PRIVATE_CODES:
            return Status.PRIVATE
        if self.online >= 0:
            check_unknown_status(
                str(self.online), _CC_KNOWN_ONLINE_CODES,
                "CC", username, logger,
            )
            return Status.PRIVATE
        return Status.UNKNOWN


class CamsCom(Bot):
    site: str = 'CamsCom'
    siteslug: str = 'CC'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_red", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://cams.com/{self.username}"

    def getVideoUrl(self) -> str:
        """Get the video stream URL."""
        return f'https://camscdn.cams.com/camscdn/cdn-{self.username.lower()}.m3u8'

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            response = self.session.get(
                f'https://beta-api.cams.com/models/stream/{self.username}/',
                timeout=30,
                bucket='status'
            )

            if response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.ERROR

            self.lastInfo = response.json()
            info = CCModelInfo.from_response(self.lastInfo, self.username, self.logger)
            return info.to_bot_status(self.username, self.logger)

        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error checking status: {e}")
            return Status.ERROR
        except (KeyError, ValueError) as e:
            self.logger.error(f"Error parsing response: {e}")
            return Status.ERROR
        except Exception as e:
            self.logger.error(f"Unexpected error: {e}")
            return Status.ERROR

    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False


# Known online flag states:
# 0: Offline
# 1: Public
# 2: Nude show
# 3: Private
# 4: Admin/Exclusive
# 6: Ticket show
# 7: Voyeur
# 10: Party
# 11: Goal up
# 12: Goal down
# 13: Group
# 14: C2C

