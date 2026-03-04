import requests
from dataclasses import dataclass
from typing import Optional, Dict, Any, Tuple, List

from streamonitor.bot import Bot
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ── Cherry.tv expected keys & known statuses ────────────────────────────
_CHTV_EXPECTED_KEYS: dict = {
    "": frozenset({"data"}),
    "data": frozenset({"streamer"}),
}
_CHTV_BROADCAST_KEYS: dict = {
    "": frozenset({"showStatus", "pullUrl"}),
}
_CHTV_KNOWN_STATUSES: frozenset = frozenset({"public", "private"})


@dataclass(frozen=True, slots=True)
class CHTVModelInfo:
    """Typed reader for the Cherry.tv GraphQL ``findStreamerBySlug`` response."""

    streamer_exists: bool
    broadcast_exists: bool
    show_status: str

    @classmethod
    def from_response(cls, data: dict, username: str = "", logger=None) -> "CHTVModelInfo":
        check_unknown_fields(data, _CHTV_EXPECTED_KEYS, "CHTV", username, logger)
        streamer = (data.get("data") or {}).get("streamer")
        if not streamer:
            return cls(streamer_exists=False, broadcast_exists=False, show_status="")
        broadcast = streamer.get("broadcast")
        if not broadcast:
            return cls(streamer_exists=True, broadcast_exists=False, show_status="")
        check_unknown_fields(broadcast, _CHTV_BROADCAST_KEYS, "CHTV", username, logger)
        return cls(
            streamer_exists=True,
            broadcast_exists=True,
            show_status=(broadcast.get("showStatus", "") or "").lower(),
        )

    def to_bot_status(self, username: str = "", logger=None) -> "Status":
        if not self.streamer_exists:
            return Status.NOTEXIST
        if not self.broadcast_exists:
            return Status.OFFLINE
        s = self.show_status
        if s == "public":
            return Status.PUBLIC
        if s == "private":
            return Status.PRIVATE
        check_unknown_status(s, _CHTV_KNOWN_STATUSES, "CHTV", username, logger)
        return Status.UNKNOWN


class CherryTV(Bot):
    site: str = 'Cherry.tv'
    siteslug: str = 'CHTV'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_magenta", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.cherry.tv/{self.username}"

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo or 'broadcast' not in self.lastInfo:
            return None
            
        broadcast = self.lastInfo.get('broadcast')
        if not broadcast or 'pullUrl' not in broadcast:
            return None
            
        return self.getWantedResolutionPlaylist(broadcast['pullUrl'])

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            operationName = 'findStreamerBySlug'
            variables = f'{{"slug": "{self.username}"}}'
            extensions = '{"persistedQuery":{"version":1,"sha256Hash":"1fd980c874484de0b139ef4a67c867200a87f44aa51caf54319e93a4108a7510"}}'

            response = self.session.get(
                f'https://api.cherry.tv/graphql?operationName={operationName}&variables={variables}&extensions={extensions}',
                headers=self.headers,
                timeout=30,
                bucket='status'
            )

            if response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.ERROR

            data = response.json()
            info = CHTVModelInfo.from_response(data, self.username, self.logger)

            # Store streamer data for getVideoUrl()
            self.lastInfo = (data.get("data") or {}).get("streamer")

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
