import requests
from typing import Optional, Dict, Any, Tuple, List
from streamonitor.bot import Bot
from streamonitor.enums import Status


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
            
            if 'data' not in data or 'streamer' not in data['data']:
                return Status.ERROR
                
            self.lastInfo = data['data']['streamer']
            
            if not self.lastInfo:
                return Status.NOTEXIST
                
            broadcast = self.lastInfo.get('broadcast')
            if not broadcast:
                return Status.OFFLINE
                
            show_status = broadcast.get('showStatus', '').lower()
            if show_status == 'public':
                return Status.PUBLIC
            elif show_status == 'private':
                return Status.PRIVATE
            else:
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
