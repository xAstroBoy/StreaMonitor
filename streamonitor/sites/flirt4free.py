import json
from dataclasses import dataclass
from typing import Optional, Dict, Any, Union, Tuple, List

import requests
from streamonitor.bot import RoomIdBot
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ── Flirt4Free expected keys & known statuses ────────────────────────
_F4F_STREAM_KEYS: dict = {"": frozenset({"code", "data"})}
_F4F_ROOM_KEYS: dict = {"": frozenset({"config"})}
_F4F_KNOWN_CODES: frozenset = frozenset({"0", "44"})
_F4F_KNOWN_ROOM_STATUSES: frozenset = frozenset({"O", "P", "F"})


@dataclass(frozen=True, slots=True)
class F4FModelInfo:
    """Combined info from Flirt4Free stream-urls + room-interface APIs."""

    code: int         # from stream-urls: 0 = OK, 44 = NOTEXIST, -1 = missing
    room_status: str  # from room-interface: O/P/F, "" if not yet fetched

    @classmethod
    def from_stream_data(cls, data: dict, username: str = "", logger=None) -> "F4FModelInfo":
        check_unknown_fields(data, _F4F_STREAM_KEYS, "F4F", username, logger)
        code = data.get("code")
        try:
            code = int(code) if code is not None else -1
        except (ValueError, TypeError):
            code = -1
        return cls(code=code, room_status="")

    @classmethod
    def from_room_data(cls, code: int, data: dict, username: str = "", logger=None) -> "F4FModelInfo":
        check_unknown_fields(data, _F4F_ROOM_KEYS, "F4F", username, logger)
        rs = (data.get("config") or {}).get("room", {}).get("status", "")
        return cls(code=code, room_status=rs)

    def to_bot_status(self, username: str = "", logger=None) -> "Status":
        if self.code == 44:
            return Status.NOTEXIST
        if self.room_status == "O":
            return Status.PUBLIC
        if self.room_status == "P":
            return Status.PRIVATE
        if self.room_status == "F":
            return Status.OFFLINE
        if self.code == 0 and self.room_status:
            check_unknown_status(self.room_status, _F4F_KNOWN_ROOM_STATUSES, "F4F", username, logger)
        elif self.code not in (0, 44, -1):
            check_unknown_status(str(self.code), _F4F_KNOWN_CODES, "F4F", username, logger)
        return Status.UNKNOWN


# Site of Hungarian group AdultPerformerNetwork
class Flirt4Free(RoomIdBot):
    site: str = 'Flirt4Free'
    siteslug: str = 'F4F'
    models: Dict[str, Dict[str, Any]] = {}

    def __init__(self, username: str, room_id: Optional[int] = None) -> None:
        if not room_id:
            room_id = self.getRoomId(username)
        super().__init__(username, room_id=str(room_id) if room_id else None)
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
            info = F4FModelInfo.from_stream_data(stream_data, self.username, self.logger)

            if info.code == 44:
                return info.to_bot_status(self.username, self.logger)

            if info.code == 0:
                try:
                    room_response = self.session.get(
                        f'https://www.flirt4free.com/ws/rooms/chat-room-interface.php?a=login_room&model_id={self.room_id}',
                        timeout=30,
                        bucket='status'
                    )

                    if room_response.status_code == 200:
                        room_data = room_response.json()
                        info = F4FModelInfo.from_room_data(info.code, room_data, self.username, self.logger)
                    else:
                        self.logger.warning(f"Room status check failed with HTTP {room_response.status_code}")

                except Exception as e:
                    self.logger.error(f"Error checking room status: {e}")
                    return Status.ERROR

            return info.to_bot_status(self.username, self.logger)

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
