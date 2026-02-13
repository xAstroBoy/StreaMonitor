import requests
from typing import Optional, Tuple, List
from streamonitor.bot import Bot
from streamonitor.enums import Status


class BongaCams(Bot):
    site: str = 'BongaCams'
    siteslug: str = 'BC'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("yellow", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://bongacams.com/{self.username}"
    
    def getPlaylistUrl(self) -> Optional[str]:
        """Get the HLS playlist URL."""
        if not self.lastInfo or 'localData' not in self.lastInfo:
            return None
            
        video_server_url = self.lastInfo['localData'].get('videoServerUrl')
        if not video_server_url:
            return None
            
        # Ensure URL has protocol
        if not video_server_url.startswith('http'):
            video_server_url = "https:" + video_server_url
            
        return f"{video_server_url}/hls/stream_{self.username}/playlist.m3u8"

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        playlist_url = self.getPlaylistUrl()
        if not playlist_url:
            return None
        return self.getWantedResolutionPlaylist(playlist_url)

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            headers = self.headers.copy()
            headers.update({
                'Content-Type': 'application/x-www-form-urlencoded',
                'Referer': f'https://de.bongacams.net/{self.username}',
                'Accept': 'application/json, text/javascript, */*; q=0.01',
                'X-Requested-With': 'XMLHttpRequest'
            })
            
            data = f'method=getRoomData&args%5B%5D={self.username}&args%5B%5D=false'
            
            response = self.session.post(
                'https://de.bongacams.net/tools/amf.php',
                data=data,
                headers=headers,
                timeout=30,
                bucket='status'
            )

            if response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.ERROR

            self.lastInfo = response.json()
            
            if self.lastInfo.get("status") == "error":
                return Status.NOTEXIST
                
            # Update username if performer changed it
            performer_data = self.lastInfo.get('performerData', {})
            actual_username = performer_data.get('username')
            if actual_username and actual_username != self.username:
                self.username = actual_username
                self.logger = self.getLogger()
                
            # Check show type
            show_type = performer_data.get('showType', '').lower()
            if show_type in ['private', 'group']:
                return Status.PRIVATE
                
            # Check if video server is available
            local_data = self.lastInfo.get('localData', {})
            if 'videoServerUrl' not in local_data:
                return Status.OFFLINE
                
            # Verify playlist is accessible
            playlist_url = self.getPlaylistUrl()
            if playlist_url:
                try:
                    playlist_response = self.session.get(
                        playlist_url,
                        timeout=10,
                        bucket='playlist'
                    )
                    
                    # Check if playlist is valid (BongaCams returns specific error response)
                    if len(playlist_response.text) <= 25 or playlist_response.status_code == 404:
                        return Status.OFFLINE
                    return Status.PUBLIC
                except Exception as e:
                    self.logger.warning(f"Error checking playlist: {e}")
                    return Status.OFFLINE
            else:
                return Status.OFFLINE
                
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
