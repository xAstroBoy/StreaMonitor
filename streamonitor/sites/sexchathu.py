import time
from typing import Optional, Dict, Any, List, Union, Tuple

import requests
from streamonitor.bot import RoomIdBot
from streamonitor.enums import Status


# Site of Hungarian group AdultPerformerNetwork
class SexChatHU(RoomIdBot):
    site: str = 'SexChatHU'
    siteslug: str = 'SCHU'
    bulk_update: bool = True

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("dark_grey", [])

    _performers_list_cache: Optional[List[Dict[str, Any]]] = None
    _performers_list_cache_timestamp: float = 0

    def __init__(self, username: str, room_id: Optional[str] = None) -> None:
        # Initialize room_id first
        if not room_id:
            if username.isnumeric():
                room_id = username
            else:
                room_id = self._findRoomId(username)
                if not room_id:
                    super().__init__(username, room_id=None)
                    self.sc = Status.NOTEXIST
                    return
                
        super().__init__(username, room_id=room_id)
        self.url = self.getWebsiteURL()

    def _findRoomId(self, username: str) -> Optional[str]:
        """Find room ID for a given username."""
        try:
            # Check if cache is expired or doesn't exist
            if (SexChatHU._performers_list_cache_timestamp < time.time() - 60 * 60 or 
                SexChatHU._performers_list_cache is None):  # Cache for 1 hour
                
                response = self.session.get(
                    'https://sexchat.hu/ajax/api/roomList/babes',
                    headers=self.headers,
                    timeout=30,
                    bucket='auth'
                )
                
                if response.status_code != 200:
                    self.logger.warning(f"Failed to get performers list: HTTP {response.status_code}")
                    return None
                    
                SexChatHU._performers_list_cache = response.json()
                SexChatHU._performers_list_cache_timestamp = time.time()
                
            # Search for performer
            if SexChatHU._performers_list_cache:
                for performer in SexChatHU._performers_list_cache:
                    if isinstance(performer, dict):
                        if performer.get('screenname') == username:
                            # Update username to match exact case
                            self.username = performer['screenname']
                            return str(performer.get('perfid', ''))
            return None
            
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error finding room ID: {e}")
            return None
        except Exception as e:
            self.logger.error(f"Error finding room ID: {e}")
            return None

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://sexchat.hu/mypage/{self.room_id}/{self.username}/chat"

    def export(self) -> Dict[str, Any]:
        """Export bot data including room_id."""
        data = super().export()
        data['room_id'] = getattr(self, 'room_id', None)
        return data

    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo or 'onlineParams' not in self.lastInfo:
            return None
            
        try:
            online_params = self.lastInfo['onlineParams']
            mode_specific = online_params.get('modeSpecific', {})
            main = mode_specific.get('main', {})
            hls = main.get('hls', {})
            address = hls.get('address', '')
            
            if not address:
                return None
                
            if not address.startswith('http'):
                address = "https:" + address
                
            return self.getWantedResolutionPlaylist(address)
            
        except (KeyError, TypeError) as e:
            self.logger.error(f"Error getting video URL: {e}")
            return None

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        if not hasattr(self, 'room_id') or not self.room_id:
            return Status.NOTEXIST
            
        try:
            response = self.session.get(
                f'https://chat.a.apn2.com/chat-api/index.php/room/getRoom?tokenID=guest&roomID={self.room_id}',
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

            if not self.lastInfo.get("active"):
                return Status.NOTEXIST
                
            online_status = self.lastInfo.get("onlineStatus", "").lower()
            
            if online_status == "free":
                # Check if HLS stream is available
                online_params = self.lastInfo.get('onlineParams', {})
                mode_specific = online_params.get('modeSpecific', {})
                main = mode_specific.get('main', {})
                
                if 'hls' in main:
                    return Status.PUBLIC
                else:
                    return Status.PRIVATE
            elif online_status in ['vip', 'group', 'priv']:
                return Status.PRIVATE
            elif online_status == "offline":
                return Status.OFFLINE
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

    def getUsernameFromRoomId(self, room_id: str) -> Optional[str]:
        """Resolve username from SexChatHU perfid."""
        babes = self._getBabesList()
        if babes:
            for babe in babes:
                if isinstance(babe, dict) and str(babe.get('perfid', '')) == str(room_id):
                    self.username = babe['screenname']
                    return babe['screenname']
        return None

    def getRoomIdFromUsername(self, username: str) -> Optional[str]:
        """Resolve room_id from SexChatHU username."""
        return self._findRoomId(username)

    @classmethod
    def _getBabesList(cls) -> Optional[List[Dict[str, Any]]]:
        """Get the performers list, using cache if available."""
        if (cls._performers_list_cache_timestamp < time.time() - 60 * 60 or
                cls._performers_list_cache is None):
            try:
                r = requests.get(
                    'https://sexchat.hu/ajax/api/roomList/babes',
                    timeout=30
                )
                if r.status_code == 200:
                    cls._performers_list_cache = r.json()
                    cls._performers_list_cache_timestamp = time.time()
            except Exception:
                pass
        return cls._performers_list_cache

    @classmethod
    def getStatusBulk(cls, streamers) -> None:
        """Bulk status update using the performers list."""
        babes = cls._getBabesList()
        if not babes:
            return

        babe_map = {}
        for babe in babes:
            if isinstance(babe, dict):
                sn = babe.get('screenname', '').lower()
                if sn:
                    babe_map[sn] = babe

        for streamer in streamers:
            babe = babe_map.get(streamer.username.lower())
            if not babe:
                if streamer.sc not in (Status.PUBLIC, Status.PRIVATE, Status.RESTRICTED):
                    streamer.setStatus(Status.OFFLINE)
                continue
            online_status = babe.get('onlineStatus', '').lower()
            if online_status == 'free':
                if streamer.sc in (Status.PUBLIC, Status.RESTRICTED):
                    continue
                status = streamer.getStatus()
            elif online_status in ('vip', 'group', 'priv'):
                status = Status.PRIVATE
            elif online_status == 'offline':
                status = Status.OFFLINE
            else:
                status = Status.UNKNOWN
            streamer.setStatus(status)
