import requests
from typing import Optional, Dict, List, Any, Union
from streamonitor.bot import Bot
from streamonitor.enums import Status


class CamSoda(Bot):
    site: str = 'CamSoda'
    siteslug: str = 'CS'
    API_BASE: str = "https://www.camsoda.com/api/v1/chat/react"

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.vr = False  # CamSoda doesn't have VR
        self.url = self.getWebsiteURL()
        self.lastInfo: Dict[str, Any] = {}
    
    def get_site_color(self):
        """Return the color scheme for this site"""
        return ("blue", [])
    
    # ──────────────── Core API ────────────────
    def fetchInfo(self) -> Dict[str, Any]:
        """Fetch raw JSON info for the performer."""
        try:
            response = self.session.get(
                f"{self.API_BASE}/{self.username}",
                headers=self.headers,
                timeout=30,
                bucket='status'
            )
            
            if response.status_code == 403:
                self.logger.warning(f"Rate limited for user {self.username}")
                return {"__status__": Status.RATELIMIT}
            elif response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return {"__status__": Status.UNKNOWN}
                
            data = response.json()
            self.lastInfo = data
            return data
            
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error fetching info: {e}")
            return {"__status__": Status.ERROR}
        except (KeyError, ValueError) as e:
            self.logger.error(f"Error parsing response: {e}")
            return {"__status__": Status.ERROR}
        except Exception as e:
            self.logger.error(f"Unexpected error: {e}")
            return {"__status__": Status.ERROR}

    # ──────────────── Convenience methods ────────────────
    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.camsoda.com/{self.username}"

    def getChatStatus(self) -> Optional[str]:
        """Return 'online', 'offline', etc. from chat.status."""
        return self.lastInfo.get("chat", {}).get("status")

    def getMode(self) -> Optional[str]:
        """Return 'public', 'private', 'offline' from API 'mode'."""
        return self.lastInfo.get("mode")

    def getStreamStatus(self) -> Optional[int]:
        """Return numeric status from stream.status (1 = live)."""
        return self.lastInfo.get("stream", {}).get("status")

    def getStreamToken(self) -> Optional[str]:
        """Get the stream token."""
        return self.lastInfo.get("stream", {}).get("token")

    def getStreamName(self) -> Optional[str]:
        """Get the stream name."""
        return self.lastInfo.get("stream", {}).get("stream_name")

    def getEdgeServers(self) -> List[str]:
        """Get list of edge servers."""
        return self.lastInfo.get("stream", {}).get("edge_servers", [])

    def getBio(self) -> Optional[str]:
        """Get user bio/about me."""
        return self.lastInfo.get("userBio", {}).get("aboutMe")

    def getWishlist(self) -> Optional[str]:
        """Get user wishlist."""
        return self.lastInfo.get("userBio", {}).get("wishList")

    def getTags(self) -> List[str]:
        """Get list of user tags."""
        tags = self.lastInfo.get("tagListNew", [])
        return [t.get("name", "") for t in tags if isinstance(t, dict) and t.get("name")]

    def getMediaList(self) -> List[Dict[str, Any]]:
        """Return all pictures/videos with metadata."""
        return self.lastInfo.get("userMediaList", [])

    def getTopMedia(self) -> List[Dict[str, Any]]:
        """Get top media list."""
        return self.lastInfo.get("userMediaTopList", [])

    def getFollowerCount(self) -> int:
        """Get follower count."""
        return self.lastInfo.get("user", {}).get("follower", {}).get("countTotal", 0)

    # ──────────────── Status evaluation ────────────────
    def getStatus(self) -> Status:
        """Check the current status of the stream."""
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
    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        if not self.lastInfo:
            return None
            
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
        master_url = f"{base}/{stream_name}_v1/index.ll.m3u8"

        # Let the shared resolution selector handle variant picking
        return self.getWantedResolutionPlaylist(master_url)

    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False
