import re
import requests
from typing import Optional, Set
from streamonitor.bot import Bot
from streamonitor.enums import Status, Gender


class Chaturbate(Bot):
    site: str = 'Chaturbate'
    siteslug: str = 'CB'
    bulk_update: bool = True

    _GENDER_MAP = {
        'f': Gender.FEMALE,
        'm': Gender.MALE,
        's': Gender.TRANS,
        'c': Gender.BOTH,
    }

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.vr = False  # Chaturbate doesn't have VR
        self.sleep_on_offline = 30
        self.sleep_on_error = 60
        self._max_consecutive_errors = 50  # Chaturbate can be flaky, allow more retries
        self.url = self.getWebsiteURL()
    
    def get_site_color(self):
        """Return the color scheme for this site"""
        return ("magenta", [])
    
    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.chaturbate.com/{self.username}"
    
    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        # If bulk_update is active, we need to fetch our own status for the URL
        if self.bulk_update:
            self.getStatus()
        # If lastInfo is missing or stale, try to refresh it
        if not self.lastInfo or 'url' not in self.lastInfo:
            self.logger.debug("lastInfo missing URL, refreshing status...")
            status = self.getStatus()
            if status != Status.PUBLIC:
                self.logger.warning(f"Cannot get video URL - status is {status}")
                return None
            if not self.lastInfo or 'url' not in self.lastInfo:
                self.logger.error("Still no URL after refresh")
                return None
            
        url = self.lastInfo['url']
        
        # Use CMAF if available for better streaming
        if self.lastInfo.get('cmaf_edge'):
            url = url.replace('playlist.m3u8', 'playlist_sfm4s.m3u8')
            url = re.sub(r'live-.+amlst', 'live-c-fhls/amlst', url)

        return self.getWantedResolutionPlaylist(url)

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        headers = {
            "X-Requested-With": "XMLHttpRequest",
            "User-Agent": self.headers.get("User-Agent", "Mozilla/5.0")
        }
        data = {
            "room_slug": self.username, 
            "bandwidth": "high"
        }

        try:
            response = self.session.post(
                "https://chaturbate.com/get_edge_hls_url_ajax/",
                headers=headers,
                data=data,
                timeout=30,
                bucket='status'
            )
            
            if response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                # Treat server errors as temporary (ratelimit) not permanent errors
                if response.status_code >= 500 or response.status_code == 429:
                    return Status.RATELIMIT
                elif response.status_code == 404:
                    return Status.NOTEXIST
                return Status.ERROR
                
            self.lastInfo = response.json()

            room_status = self.lastInfo.get("room_status", "").lower()
            status = self._parseStatus(room_status)
            if status == Status.PUBLIC and not self.lastInfo.get('url'):
                status = Status.RESTRICTED
                
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error checking status: {e}")
            status = Status.RATELIMIT
        except (KeyError, ValueError) as e:
            self.logger.error(f"Error parsing response: {e}")
            status = Status.ERROR
        except Exception as e:
            self.logger.error(f"Unexpected error: {e}")
            status = Status.ERROR

        self.ratelimit = status == Status.RATELIMIT
        return status

    @staticmethod
    def _parseStatus(room_status: str) -> Status:
        """Parse room status string into Status enum."""
        if room_status == "public":
            return Status.PUBLIC
        elif room_status in ("private", "hidden"):
            return Status.PRIVATE
        elif room_status == "offline":
            return Status.OFFLINE
        else:
            return Status.OFFLINE

    @classmethod
    def getStatusBulk(cls, streamers: Set['Chaturbate']) -> None:
        """Bulk status update using the affiliates API."""
        session = requests.Session()
        session.headers.update(cls.headers)
        try:
            r = session.get("https://chaturbate.com/affiliates/api/onlinerooms/?format=json&wm=DkfRj", timeout=10)
            try:
                data = r.json()
            except requests.exceptions.JSONDecodeError:
                return

            data_map = {str(model['username']).lower(): model for model in data}

            for streamer in streamers:
                model_data = data_map.get(streamer.username.lower())
                if not model_data:
                    streamer.setStatus(Status.OFFLINE)
                    continue
                if model_data.get('gender'):
                    streamer.gender = cls._GENDER_MAP.get(model_data['gender'], Gender.UNKNOWN)
                if model_data.get('country'):
                    streamer.country = model_data.get('country', '').upper()
                status = cls._parseStatus(model_data.get('current_show', ''))
                if status == Status.PUBLIC:
                    if streamer.sc in (Status.PUBLIC, Status.RESTRICTED):
                        continue
                    status = streamer.getStatus()
                streamer.setStatus(status)
        except Exception as e:
            # Silently fail for bulk — individual bots will still poll on their own
            pass

    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False
