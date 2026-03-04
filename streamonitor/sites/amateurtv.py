import requests
from dataclasses import dataclass
from typing import Optional, List, Dict, Any, Tuple
from streamonitor.bot import Bot
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ---------------------------------------------------------------------------
# ATVModelInfo – Foolproof JSON reader for AmateurTV
# ---------------------------------------------------------------------------
_ATV_KNOWN_STATUSES = frozenset({"online", "offline"})
_ATV_EXPECTED_KEYS = {
    "": frozenset({
        "status", "message", "result", "privateChatStatus", "qualities",
        "videoTechnologies", "username", "id", "slug", "age", "gender",
        "country", "languages", "tags", "description", "avatar",
        "preview", "viewers", "followers",
    }),
}


@dataclass(frozen=True, slots=True)
class ATVModelInfo:
    """Immutable snapshot parsed from AmateurTV's API."""
    status: str
    message: str
    result: str
    private_chat_status: bool   # True if privateChatStatus is not None

    @classmethod
    def from_response(cls, data: dict, username: str = "", logger=None) -> "ATVModelInfo":
        check_unknown_fields(data, _ATV_EXPECTED_KEYS, "ATV", username, logger)
        return cls(
            status=(data.get("status", "") or "").lower(),
            message=data.get("message", "") or "",
            result=data.get("result", "") or "",
            private_chat_status=data.get("privateChatStatus") is not None,
        )

    def to_bot_status(self, username: str = "") -> Status:
        if self.message == "NOT_FOUND":
            return Status.NOTEXIST
        if self.result == "KO":
            return Status.ERROR
        check_unknown_status(self.status, _ATV_KNOWN_STATUSES, "ATV", username)
        if self.status == "online":
            return Status.PRIVATE if self.private_chat_status else Status.PUBLIC
        if self.status == "offline":
            return Status.OFFLINE
        return Status.UNKNOWN


class AmateurTV(Bot):
    site: str = 'AmateurTV'
    siteslug: str = 'ATV'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_grey", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://amateur.tv/{self.username}"

    def getPlaylistVariants(self, url: Optional[str] = None) -> List[Dict[str, Any]]:
        """Get available video quality variants."""
        sources = []
        if not self.lastInfo or 'qualities' not in self.lastInfo:
            return sources
            
        for resolution in self.lastInfo['qualities']:
            try:
                width, height = resolution.split('x')
                video_url = self.lastInfo.get('videoTechnologies', {}).get('fmp4', '')
                if video_url:
                    sources.append({
                        'url': f"{video_url}&variant={height}",
                        'resolution': (int(width), int(height)),
                        'frame_rate': None,
                        'bandwidth': None
                    })
            except (ValueError, KeyError) as e:
                self.logger.warning(f"Error parsing resolution {resolution}: {e}")
                continue
        return sources

    def getVideoUrl(self) -> Optional[str]:
        """Get the video URL for the selected quality."""
        return self.getWantedResolutionPlaylist(None)

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            headers = self.headers.copy()
            headers.update({
                'Content-Type': 'application/json',
                'Referer': 'https://amateur.tv/'
            })
            
            response = self.session.get(
                f'https://www.amateur.tv/v3/readmodel/show/{self.username}/en',
                headers=headers,
                timeout=30,
                bucket='status'
            )

            if response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.ERROR

            raw = response.json()
            self.lastInfo = raw

            info = ATVModelInfo.from_response(raw, self.username, self.logger)
            return info.to_bot_status(self.username)
                
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
