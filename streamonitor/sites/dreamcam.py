import requests
from typing import Optional, Dict, Any, List, Tuple

from streamonitor.bot import Bot
from streamonitor.enums import Status


class DreamCam(Bot):
    site: str = 'DreamCam'
    siteslug: str = 'DC'

    _stream_type: str = 'video2D'

    def __init__(self, username: str) -> None:
        super().__init__(username)

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_blue", [])
        self.url = self.getWebsiteURL()

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://dreamcamtrue.com/{self.username}"

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo or 'streams' not in self.lastInfo:
            return None
            
        streams = self.lastInfo.get('streams', [])
        
        for stream in streams:
            if isinstance(stream, dict):
                if stream.get('streamType') == self._stream_type:
                    if stream.get('status') == 'online':
                        return stream.get('url')
        return None

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            response = self.session.get(
                f'https://bss.dreamcamtrue.com/api/clients/v1/broadcasts/models/{self.username}',
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
            
            broadcast_status = self.lastInfo.get("broadcastStatus", "").lower()

            if broadcast_status == "public":
                return Status.PUBLIC
            elif broadcast_status == "private":
                return Status.PRIVATE
            elif broadcast_status in ["away", "offline"]:
                return Status.OFFLINE
            else:
                self.logger.warning(f'Got unknown status: {broadcast_status}')
                return Status.UNKNOWN
                
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
