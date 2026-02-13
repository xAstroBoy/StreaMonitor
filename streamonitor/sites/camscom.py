import requests
from typing import Optional, Tuple, List
from streamonitor.bot import Bot
from streamonitor.enums import Status


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
            
            # Check if stream_name exists to verify user existence
            if 'stream_name' not in self.lastInfo:
                return Status.NOTEXIST
                
            online_status = self.lastInfo.get('online')
            
            # Handle different online status values
            if online_status == '0' or online_status == 0:
                return Status.OFFLINE
            elif online_status == '1' or online_status == 1:
                return Status.PUBLIC
            elif online_status == '2' or online_status == 2:  # Nude show
                return Status.PUBLIC
            elif online_status in ['3', '4', '7', '13', '14', 3, 4, 7, 13, 14]:  # Private/Group/Voyeur/C2C
                return Status.PRIVATE
            elif online_status in ['6', '10', '11', '12', 6, 10, 11, 12]:  # Ticket/Party/Goal shows
                return Status.PUBLIC
            elif online_status is not None:
                # Any other non-null value indicates some form of private show
                return Status.PRIVATE
                
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

