from dataclasses import dataclass

from streamonitor.bot import RoomIdBot
from streamonitor.enums import Status
from streamonitor.model_info_base import check_unknown_fields, check_unknown_status


# ── FanslyLive expected keys & known statuses ────────────────────────────
_FL_EXPECTED_KEYS: dict = {
    "": frozenset({"success", "response"}),
}
_FL_RESPONSE_KEYS: dict = {
    "": frozenset({"stream"}),
}
_FL_STREAM_KEYS: dict = {
    "": frozenset({"status", "access", "playbackUrl"}),
}
_FL_KNOWN_STREAM_STATUSES: frozenset = frozenset({"2"})  # 2 = live


@dataclass(frozen=True, slots=True)
class FLModelInfo:
    """Typed reader for the FanslyLive streaming channel response."""

    success: bool
    has_response: bool
    has_stream: bool
    stream_status: int   # 2 = live, -1 = missing
    stream_access: bool
    has_playback_url: bool

    @classmethod
    def from_response(cls, data: dict, username: str = "", logger=None) -> "FLModelInfo":
        check_unknown_fields(data, _FL_EXPECTED_KEYS, "FL", username, logger)
        success = data.get("success") is True
        resp = data.get("response")
        if not resp:
            return cls(success=success, has_response=False, has_stream=False,
                       stream_status=-1, stream_access=False, has_playback_url=False)
        if isinstance(resp, dict):
            check_unknown_fields(resp, _FL_RESPONSE_KEYS, "FL", username, logger)
        stream = resp.get("stream") if isinstance(resp, dict) else None
        if not stream:
            return cls(success=success, has_response=True, has_stream=False,
                       stream_status=-1, stream_access=False, has_playback_url=False)
        if isinstance(stream, dict):
            check_unknown_fields(stream, _FL_STREAM_KEYS, "FL", username, logger)
        raw_ss = stream.get("status") if isinstance(stream, dict) else None
        try:
            ss = int(raw_ss) if raw_ss is not None else -1
        except (ValueError, TypeError):
            ss = -1
        return cls(
            success=success,
            has_response=True,
            has_stream=True,
            stream_status=ss,
            stream_access=bool(stream.get("access")),
            has_playback_url="playbackUrl" in stream,
        )

    def to_bot_status(self, username: str = "", logger=None) -> "Status":
        if not self.success:
            return Status.UNKNOWN
        if not self.has_response:
            return Status.NOTEXIST
        if not self.has_stream:
            return Status.UNKNOWN
        if self.stream_status == 2:
            if self.stream_access and self.has_playback_url:
                return Status.PUBLIC
            return Status.PRIVATE
        if self.stream_status >= 0:
            check_unknown_status(
                str(self.stream_status), _FL_KNOWN_STREAM_STATUSES,
                "FL", username, logger,
            )
        return Status.UNKNOWN


class FanslyLive(RoomIdBot):
    site = 'FanslyLive'
    siteslug = 'FL'

    def getWebsiteURL(self):
        return "https://fansly.com/live/" + self.username

    def getVideoUrl(self):
        return self.getWantedResolutionPlaylist(self.lastInfo['stream']['playbackUrl'])

    def getUsernameFromRoomId(self, room_id):
        r = self.session.get(f'https://apiv3.fansly.com/api/v1/account?ids={room_id}&ngsw-bypass=true')
        data = r.json()
        for streamer in data.get('response', []):
            if streamer.get('id') == room_id:
                self.username = streamer['username']
                return streamer.get('username')
        return None

    def getRoomIdFromUsername(self, username):
        r = self.session.get(f'https://apiv3.fansly.com/api/v1/account?usernames={username}&ngsw-bypass=true')
        data = r.json()
        for streamer in data.get('response', []):
            if streamer.get('username').lower() == username.lower():
                self.username = streamer['username']
                return streamer.get('id')
        return None

    def getStatus(self):
        if self.room_id is None:
            self.room_id = self.getRoomIdFromUsername(self.username)
        if self.room_id is None:
            return Status.NOTEXIST

        r = self.session.get(f'https://apiv3.fansly.com/api/v1/streaming/channel/{self.room_id}?ngsw-bypass=true')
        data = r.json()

        info = FLModelInfo.from_response(data, self.username, self.logger)

        # Store response for getVideoUrl()
        self.lastInfo = data.get('response')

        return info.to_bot_status(self.username, self.logger)
