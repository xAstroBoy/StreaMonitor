import json
from typing import Optional, Dict, Any, Union, Tuple, List

import requests
from streamonitor.bot import Bot
from streamonitor.enums import Status


# Site of Hungarian group AdultPerformerNetwork
class Flirt4Free(Bot):
    site: str = 'Flirt4Free'
    siteslug: str = 'F4F'
    models: Dict[str, Dict[str, Any]] = {}

    def __init__(self, username: str, room_id: Optional[int] = None) -> None:
        self.room_id = room_id if room_id else self.getRoomId(username)
        super().__init__(username)
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("light_yellow", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.flirt4free.com/?model={self.username}"

    def getRoomId(self, username: str) -> Optional[int]:
        """Get the room ID for a username."""
        if username in Flirt4Free.models:
            return Flirt4Free.models[username]['model_id']

        try:
            response = self.session.get(
                f'https://www.flirt4free.com/?model={username}',
                timeout=30,
                bucket='status'
            )

            start = b'window.__homePageData__ = '

            if response.content.find(start) == -1:
                return None

            content = response.content
            j = content[content.find(start) + len(start):]
            j = j[j.find(b'['):j.find(b'],\n') + 1]
            j = j[j.find(b'['):j.rfind(b',')] + b']'

            try:
                m = json.loads(j)
                Flirt4Free.models = {v['model_seo_name']: v for v in m if isinstance(v, dict)}
            except (json.JSONDecodeError, KeyError) as e:
                self.logger.error(f'Failed to parse JSON: {e}')
                return None

            if username in Flirt4Free.models:
                return Flirt4Free.models[username]['model_id']
            return None
            
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error getting room ID: {e}")
            return None
        except Exception as e:
            self.logger.error(f"Unexpected error getting room ID: {e}")
            return None

    def export(self) -> Dict[str, Any]:
        """Export bot data including room_id."""
        data = super().export()
        data['room_id'] = self.room_id
        return data

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo or 'data' not in self.lastInfo:
            return None
            
        hls_data = self.lastInfo.get('data', {}).get('hls', [])
        if not hls_data or not hls_data[0].get('url'):
            return None
            
        url = hls_data[0]['url']
        if not url.startswith('http'):
            url = "https:" + url
            
        return self.getWantedResolutionPlaylist(url)

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        if not self.room_id:
            return Status.NOTEXIST
            
        try:
            # Get stream URLs
            response = self.session.get(
                f'https://www.flirt4free.com/ws/chat/get-stream-urls.php?model_id={self.room_id}',
                timeout=30,
                bucket='status'
            )
            
            if response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.ERROR
                
            stream_data = response.json()
            self.lastInfo = stream_data
            
            if stream_data.get('code') == 44:
                return Status.NOTEXIST
                
            if stream_data.get('code') == 0:
                # Get room status
                try:
                    room_response = self.session.get(
                        f'https://www.flirt4free.com/ws/rooms/chat-room-interface.php?a=login_room&model_id={self.room_id}',
                        timeout=30,
                        bucket='status'
                    )
                    
                    if room_response.status_code != 200:
                        self.logger.warning(f"Room status check failed with HTTP {room_response.status_code}")
                        return Status.UNKNOWN
                        
                    room_data = room_response.json()
                    
                    if 'config' not in room_data:
                        return Status.UNKNOWN
                        
                    room_status = room_data.get('config', {}).get('room', {}).get('status')
                    
                    if room_status == 'O':
                        return Status.PUBLIC
                    elif room_status == 'P':
                        return Status.PRIVATE
                    elif room_status == 'F':
                        return Status.OFFLINE
                        
                except Exception as e:
                    self.logger.error(f"Error checking room status: {e}")
                    return Status.ERROR

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


Bot.loaded_sites.add(Flirt4Free)
