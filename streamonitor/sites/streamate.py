import requests
from typing import Optional, List, Dict, Any, Tuple
from streamonitor.bot import Bot
from streamonitor.enums import Status


class StreaMate(Bot):
    site: str = 'StreaMate'
    siteslug: str = 'SM'
    aliases: List[str] = ['pornhublive']

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("grey", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://streamate.com/cam/{self.username}"

    def getPlaylistVariants(self, url: Optional[str]) -> List[Dict[str, Any]]:
        """Get playlist variants from the manifest."""
        sources = []
        
        if not self.lastInfo or 'formats' not in self.lastInfo:
            return sources
            
        formats = self.lastInfo.get('formats', {})
        mp4_hls = formats.get('mp4-hls', {})
        encodings = mp4_hls.get('encodings', [])
        
        # formats: mp4-rtmp, mp4-hls, mp4-ws
        for source in encodings:
            if isinstance(source, dict):
                sources.append({
                    'url': source.get('location', ''),
                    'resolution': (source.get('videoWidth', 0), source.get('videoHeight', 0)),
                    'frame_rate': None,
                    'bandwidth': None
                })
        return sources

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        return self.getWantedResolutionPlaylist(None)

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            headers = self.headers.copy()
            headers.update({
                'Content-Type': 'application/json',
                'Referer': 'https://streamate.com/'
            })
            
            response = self.session.get(
                f'https://manifest-server.naiadsystems.com/live/s:{self.username}.json?last=load&format=mp4-hls',
                headers=headers,
                timeout=30,
                bucket='status'
            )

            if response.status_code == 200:
                try:
                    self.lastInfo = response.json()
                    return Status.PUBLIC
                except (ValueError, KeyError) as e:
                    self.logger.error(f"Error parsing response: {e}")
                    return Status.ERROR
            elif response.status_code == 404:
                return Status.NOTEXIST
            elif response.status_code == 403:
                return Status.PRIVATE
            else:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                # Map status code to Status enum
                if hasattr(Status, str(response.status_code)):
                    return Status(response.status_code)
                return Status.UNKNOWN
                
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error checking status: {e}")
            return Status.ERROR
        except Exception as e:
            self.logger.error(f"Unexpected error: {e}")
            return Status.ERROR
    
    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False
