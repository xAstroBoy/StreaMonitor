import re
import requests
from typing import Optional
from streamonitor.bot import Bot
from streamonitor.enums import Status


class Chaturbate(Bot):
    site: str = 'Chaturbate'
    siteslug: str = 'CB'

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
            
            if room_status == "public":
                status = Status.PUBLIC
            elif room_status in ["private", "hidden"]:
                status = Status.PRIVATE
            elif room_status == "offline":
                status = Status.OFFLINE
            else:
                self.logger.warning(f"Unknown room status: {room_status}")
                status = Status.OFFLINE
                
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

    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False


Bot.loaded_sites.add(Chaturbate)
