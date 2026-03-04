import base64
import json
from dataclasses import dataclass
from typing import Optional, Dict, Any, Tuple, List

import requests
from requests.cookies import RequestsCookieJar
from streamonitor.bot import Bot
from streamonitor.downloaders.hls import getVideoNativeHLS
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ── ManyVids expected keys & known statuses ────────────────────────────
_MV_ROOMPOOL_KEYS: dict = {
    "": frozenset({"roomLocationReason", "publicAPIURL", "floorId"}),
}
_MV_STREAM_KEYS: dict = {"": frozenset({"withCredentials"})}
_MV_KNOWN_ROOM_REASONS: frozenset = frozenset({"ROOM_VALIDATION_FAILED", "ROOM_OK"})


@dataclass(frozen=True, slots=True)
class MVModelInfo:
    """Typed reader for the ManyVids roompool response."""

    room_location_reason: str

    @classmethod
    def from_response(cls, data: dict, username: str = "", logger=None) -> "MVModelInfo":
        check_unknown_fields(data, _MV_ROOMPOOL_KEYS, "MV", username, logger)
        return cls(
            room_location_reason=data.get("roomLocationReason", "") or "",
        )

    def to_bot_status(self, username: str = "", logger=None) -> "Status":
        r = self.room_location_reason
        if r == "ROOM_VALIDATION_FAILED":
            return Status.NOTEXIST
        if r == "ROOM_OK":
            return Status.PUBLIC  # caller verifies stream separately
        check_unknown_status(r, _MV_KNOWN_ROOM_REASONS, "MV", username, logger)
        return Status.UNKNOWN


class ManyVids(Bot):
    site: str = 'ManyVids'
    siteslug: str = 'MV'

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_green", [])

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.getVideo = getVideoNativeHLS
        self.stopDownloadFlag: bool = False
        self.cookies: RequestsCookieJar = RequestsCookieJar()
        self.cookieUpdater = self.updateMediaCookies
        self.cookie_update_interval: int = 120
        self.url = self.getWebsiteURL()
        self.updateSiteCookies()

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.manyvids.com/live/{self.username}"

    def requestStreamInfo(self) -> requests.Response:
        """Request stream info from the API."""
        if not self.lastInfo or 'publicAPIURL' not in self.lastInfo or 'floorId' not in self.lastInfo:
            raise ValueError("Missing API URL or floor ID in lastInfo")
            
        url = "/".join([
            self.lastInfo['publicAPIURL'], 
            str(self.lastInfo['floorId']), 
            'player-settings', 
            self.username
        ])
        
        response = self.session.get(
            url,
            headers=self.headers,
            cookies=self.cookies,
            timeout=30,
            bucket='stream'
        )
        
        if response.cookies:
            self.cookies.update(response.cookies)
        return response

    def updateMediaCookies(self) -> bool:
        """Update media cookies."""
        try:
            r = self.requestStreamInfo()
            return r.cookies is not None
        except Exception as e:
            self.logger.error(f"Error updating media cookies: {e}")
            return False

    def updateSiteCookies(self) -> None:
        """Update site cookies."""
        try:
            response = self.session.get(
                'https://www.manyvids.com/tak-live-redirect.php',
                allow_redirects=False,
                timeout=30,
                bucket='auth'
            )
            self.cookies.update(response.cookies)
        except Exception as e:
            self.logger.error(f"Error updating site cookies: {e}")

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        try:
            r = self.requestStreamInfo()
            
            policy_cookie = r.cookies.get('CloudFront-Policy')
            if not policy_cookie:
                return None
                
            # Decode CloudFront policy
            policy_data = policy_cookie.replace('_', '=')
            params = json.loads(base64.b64decode(policy_data))
            
            resource = params.get('Statement', [{}])[0].get('Resource', '')
            if not resource:
                return None
                
            url = resource[:-1] + self.username + '.m3u8'
            return self.getWantedResolutionPlaylist(url)
            
        except Exception as e:
            self.logger.error(f"Error getting video URL: {e}")
            return None

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            response = self.session.get(
                f'https://roompool.live.manyvids.com/roompool/{self.username}?private=false',
                headers=self.headers,
                timeout=30,
                bucket='status'
            )

            if response.status_code != 200:
                if response.status_code == 404:
                    return Status.NOTEXIST
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.UNKNOWN

            self.lastInfo = response.json()
            info = MVModelInfo.from_response(self.lastInfo, self.username, self.logger)

            if info.room_location_reason == "ROOM_VALIDATION_FAILED":
                return Status.NOTEXIST
            elif info.room_location_reason == "ROOM_OK":
                try:
                    stream_response = self.requestStreamInfo()
                    stream_data = stream_response.json()
                    check_unknown_fields(stream_data, _MV_STREAM_KEYS, "MV", self.username, self.logger)

                    if 'withCredentials' not in stream_data:
                        return Status.OFFLINE
                    return Status.PUBLIC

                except Exception as e:
                    self.logger.error(f"Error checking stream info: {e}")
                    return Status.ERROR

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
