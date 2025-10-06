import requests
from typing import Optional, Tuple, List
from streamonitor.bot import Bot
from streamonitor.enums import Status


class Cam4(Bot):
    site: str = 'Cam4'
    siteslug: str = 'C4'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("red", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://hu.cam4.com/{self.username}"
    
    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo or 'cdnURL' not in self.lastInfo:
            return None
        return self.getWantedResolutionPlaylist(self.lastInfo['cdnURL'])

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            headers = self.headers.copy()
            headers.update({
                "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8"
            })

            # If not currently streaming, check profile info first
            if self.sc == Status.NOTRUNNING:
                try:
                    profile_response = self.session.get(
                        f'https://hu.cam4.com/rest/v1.0/profile/{self.username}/info',
                        headers=headers,
                        timeout=30,
                        bucket='status'
                    )
                    
                    if profile_response.status_code == 403:
                        return Status.RESTRICTED
                    elif profile_response.status_code != 200:
                        self.logger.warning(f"Profile check failed with HTTP {profile_response.status_code}")
                        return Status.NOTEXIST

                    profile_data = profile_response.json()
                    if not profile_data.get('online', False):
                        return Status.OFFLINE
                        
                except Exception as e:
                    self.logger.error(f"Error checking profile: {e}")
                    return Status.ERROR

            # Check access to room
            try:
                access_response = self.session.get(
                    f'https://webchat.cam4.com/requestAccess?roomname={self.username}',
                    headers=headers,
                    timeout=30,
                    bucket='status'
                )
                
                if access_response.status_code != 200:
                    self.logger.warning(f"Access check failed with HTTP {access_response.status_code}")
                    return Status.UNKNOWN
                    
                access_data = access_response.json()
                if access_data.get('privateStream', False):
                    return Status.PRIVATE
                    
            except Exception as e:
                self.logger.error(f"Error checking room access: {e}")
                return Status.ERROR

            # Get stream info
            try:
                stream_response = self.session.get(
                    f'https://hu.cam4.com/rest/v1.0/profile/{self.username}/streamInfo',
                    headers=headers,
                    timeout=30,
                    bucket='status'
                )
                
                if stream_response.status_code == 204:
                    return Status.OFFLINE
                elif stream_response.status_code == 200:
                    self.lastInfo = stream_response.json()
                    return Status.PUBLIC
                else:
                    self.logger.warning(f"Stream info check failed with HTTP {stream_response.status_code}")
                    return Status.UNKNOWN
                    
            except Exception as e:
                self.logger.error(f"Error getting stream info: {e}")
                return Status.ERROR

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


Bot.loaded_sites.add(Cam4)
