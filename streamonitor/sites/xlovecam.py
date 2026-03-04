import requests
from dataclasses import dataclass
from typing import Optional, Dict, Any, Tuple, List

from streamonitor.bot import Bot
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ── XLoveCam expected keys & known statuses ─────────────────────────────
_XLC_EXPECTED_KEYS: dict = {
    "": frozenset({"content"}),
}
_XLC_PERFORMER_KEYS: dict = {
    "": frozenset({"enabled", "online", "hlsPlaylistFree"}),
}
_XLC_KNOWN_ONLINE_CODES: frozenset = frozenset({"0", "1"})


@dataclass(frozen=True, slots=True)
class XLCModelInfo:
    """Typed reader for the XLoveCam ``getPerformerRoom`` response."""

    has_performer: bool
    enabled: bool
    online: int  # 0 = offline, 1 = online, -1 = missing
    has_hls_free: bool

    @classmethod
    def from_response(cls, resp_data: dict, username: str = "", logger=None) -> "XLCModelInfo":
        check_unknown_fields(resp_data, _XLC_EXPECTED_KEYS, "XLC", username, logger)
        content = resp_data.get("content") or {}
        performer = content.get("performer") if isinstance(content, dict) else None
        if not performer:
            return cls(has_performer=False, enabled=False, online=-1, has_hls_free=False)
        if isinstance(performer, dict):
            check_unknown_fields(performer, _XLC_PERFORMER_KEYS, "XLC", username, logger)
        raw_online = performer.get("online")
        try:
            online = int(raw_online) if raw_online is not None else -1
        except (ValueError, TypeError):
            online = -1
        return cls(
            has_performer=True,
            enabled=bool(performer.get("enabled")),
            online=online,
            has_hls_free="hlsPlaylistFree" in performer,
        )

    def to_bot_status(self, username: str = "", logger=None) -> "Status":
        if not self.has_performer:
            return Status.UNKNOWN
        if not self.enabled:
            return Status.NOTEXIST
        if self.online == 1:
            return Status.PUBLIC if self.has_hls_free else Status.PRIVATE
        if self.online == 0:
            return Status.OFFLINE
        if self.online >= 0:
            check_unknown_status(
                str(self.online), _XLC_KNOWN_ONLINE_CODES,
                "XLC", username, logger,
            )
        return Status.UNKNOWN


class XLoveCam(Bot):
    site: str = 'XLoveCam'
    siteslug: str = 'XLC'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self._id: Optional[int] = self.getPerformerId()
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_white", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.xlovecam.com/{self.username}"

    def getPerformerId(self) -> Optional[int]:
        """Get the performer ID for the username."""
        try:
            data = {
                'config[nickname]': self.username,
                'config[favorite]': "0",
                'config[recent]': "0",
                'config[vip]': "0",
                'config[sort][id]': "35",
                'offset[from]': "0",
                'offset[length]': "35",
                'origin': "filter-chg",
                'stat': "0",
            }
            
            response = self.session.post(
                'https://www.xlovecam.com/hu/performerAction/onlineList',
                headers=self.headers,
                data=data,
                timeout=30,
                bucket='auth'
            )
            
            if not response.ok:
                self.logger.warning(f"Failed to get performer list: HTTP {response.status_code}")
                return None
                
            resp = response.json()
            
            content = resp.get('content', {})
            performer_list = content.get('performerList', [])
            
            for performer in performer_list:
                if isinstance(performer, dict):
                    nickname = performer.get('nickname', '').lower()
                    if nickname == self.username.lower():
                        return performer.get('id')
            return None
            
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error getting performer ID: {e}")
            return None
        except (KeyError, ValueError) as e:
            self.logger.error(f"Error parsing performer response: {e}")
            return None
        except Exception as e:
            self.logger.error(f"Unexpected error getting performer ID: {e}")
            return None

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo:
            return None
        return self.lastInfo.get('hlsPlaylistFree')

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        if self._id is None:
            return Status.NOTEXIST

        try:
            data = {
                'performerId': self._id,
            }

            response = self.session.post(
                'https://www.xlovecam.com/hu/performerAction/getPerformerRoom',
                headers=self.headers,
                data=data,
                timeout=30,
                bucket='status'
            )

            if not response.ok:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.UNKNOWN

            resp_data = response.json()
            info = XLCModelInfo.from_response(resp_data, self.username, self.logger)

            # Store performer data for getVideoUrl()
            self.lastInfo = (resp_data.get("content") or {}).get("performer")

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
