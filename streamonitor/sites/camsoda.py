import requests
from streamonitor.bot import Bot
from streamonitor.enums import Status


class CamSoda(Bot):
    site = 'CamSoda'
    siteslug = 'CS'
    API_BASE = "https://www.camsoda.com/api/v1/chat/react"

    def __init__(self, username):
        super().__init__(username)
        self.url = self.getWebsiteURL()
        self.lastInfo = {}
    
    # ──────────────── Core API ────────────────
    def fetchInfo(self) -> dict:
        """Fetch raw JSON info for the performer."""
        r = self.session.get(f"{self.API_BASE}/{self.username}", headers=self.headers)
        if r.status_code == 403:
            return {"__status__": Status.RATELIMIT}
        if r.status_code != 200:
            return {"__status__": Status.UNKNOWN}
        data = r.json()
        self.lastInfo = data
        return data

    # ──────────────── Convenience methods ────────────────
    def getWebsiteURL(self) -> str:
        return f"https://www.camsoda.com/{self.username}"

    def getChatStatus(self) -> str | None:
        """Return 'online', 'offline', etc. from chat.status."""
        return self.lastInfo.get("chat", {}).get("status")

    def getMode(self) -> str | None:
        """Return 'public', 'private', 'offline' from API 'mode'."""
        return self.lastInfo.get("mode")

    def getStreamStatus(self) -> int | None:
        """Return numeric status from stream.status (1 = live)."""
        return self.lastInfo.get("stream", {}).get("status")

    def getStreamToken(self) -> str | None:
        return self.lastInfo.get("stream", {}).get("token")

    def getStreamName(self) -> str | None:
        return self.lastInfo.get("stream", {}).get("stream_name")

    def getEdgeServers(self) -> list[str]:
        return self.lastInfo.get("stream", {}).get("edge_servers", [])

    def getBio(self) -> str | None:
        return self.lastInfo.get("userBio", {}).get("aboutMe")

    def getWishlist(self) -> str | None:
        return self.lastInfo.get("userBio", {}).get("wishList")

    def getTags(self) -> list[str]:
        return [t.get("name") for t in self.lastInfo.get("tagListNew", [])]

    def getMediaList(self) -> list[dict]:
        """Return all pictures/videos with metadata."""
        return self.lastInfo.get("userMediaList", [])

    def getTopMedia(self) -> list[dict]:
        return self.lastInfo.get("userMediaTopList", [])

    def getFollowerCount(self) -> int:
        return self.lastInfo.get("user", {}).get("follower", {}).get("countTotal", 0)

    # ──────────────── Status evaluation ────────────────
    def getStatus(self) -> Status:
        data = self.fetchInfo()
        if "__status__" in data:
            return data["__status__"]

        if "error" in data and data["error"] == "No username found.":
            return Status.NOTEXIST

        chat_status = self.getChatStatus()
        mode = self.getMode()
        stream_status = self.getStreamStatus()

        if chat_status == "online" and mode == "public":
            return Status.PUBLIC
        if chat_status == "online" and mode == "private":
            return Status.PRIVATE
        if chat_status == "offline":
            return Status.OFFLINE
        if stream_status == 1:
            return Status.PUBLIC

        return Status.UNKNOWN

        # ──────────────── Video URL ────────────────
    def getVideoUrl(self):
        stream = self.lastInfo.get("stream", {})
        servers = stream.get("edge_servers", [])
        stream_name = stream.get("stream_name")
        token = stream.get("token")
        if not servers or not stream_name or not token:
            return None

        base = servers[0]
        if not base.startswith("http"):
            base = "https://" + base

        # CamSoda needs the MASTER playlist (contains all variants)
        master_url = (
            f"{base}/{stream_name}_v1/index.ll.m3u8"
        )

        # Let the shared resolution selector handle variant picking
        return self.getWantedResolutionPlaylist(master_url)


    def isMobile(self) -> bool:
        return False


Bot.loaded_sites.add(CamSoda)
