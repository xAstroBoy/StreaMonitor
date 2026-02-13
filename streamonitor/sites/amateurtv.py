import requests
from typing import Optional, List, Dict, Any, Tuple
from streamonitor.bot import Bot
from streamonitor.enums import Status


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

            self.lastInfo = response.json()

            if self.lastInfo.get('message') == 'NOT_FOUND':
                return Status.NOTEXIST
                
            if self.lastInfo.get('result') == 'KO':
                return Status.ERROR
                
            stream_status = self.lastInfo.get('status')
            if stream_status == 'online':
                if self.lastInfo.get('privateChatStatus') is None:
                    return Status.PUBLIC
                else:
                    return Status.PRIVATE
            elif stream_status == 'offline':
                return Status.OFFLINE
            else:
                self.logger.warning(f"Unknown status: {stream_status}")
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
