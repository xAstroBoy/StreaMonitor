import re
import requests
import time
import random
from typing import Optional, Set
from streamonitor.bot import Bot
from streamonitor.enums import Status, Gender


class Chaturbate(Bot):
    site: str = 'Chaturbate'
    siteslug: str = 'CB'
    bulk_update: bool = True

    _GENDER_MAP = {
        'f': Gender.FEMALE,
        'm': Gender.MALE,
        's': Gender.TRANS,
        'c': Gender.BOTH,
    }

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.vr = False  # Chaturbate doesn't have VR
        self.sleep_on_offline = 15  # Shorter offline sleep
        self.sleep_on_error = 30   # Shorter error sleep
        self.sleep_on_ratelimit = 120  # Shorter rate limit sleep
        self._max_consecutive_errors = 200  # Much higher error tolerance for Chaturbate
        self._last_successful_request = time.time()
        self._request_failure_count = 0
        self._backup_endpoints = [
            "https://chaturbate.com/get_edge_hls_url_ajax/",
            "https://en.chaturbate.com/get_edge_hls_url_ajax/",
        ]
        self.url = self.getWebsiteURL()
    
    def get_site_color(self):
        """Return the color scheme for this site"""
        return ("magenta", [])
    
    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.chaturbate.com/{self.username}"
    
    def getVideoUrl(self) -> Optional[str]:
        """Get the video stream URL."""
        # If bulk_update is active, we need to fetch our own status for the URL
        if self.bulk_update:
            self.getStatus()
        # If lastInfo is missing or stale, try to refresh it
        if not self.lastInfo or 'url' not in self.lastInfo:
            self.logger.debug("lastInfo missing URL, refreshing status...")
            status = self.getStatus()
            if status != Status.PUBLIC:
                self.logger.warning(f"Cannot get video URL - status is {status}")
                return None
            if not self.lastInfo or 'url' not in self.lastInfo:
                self.logger.error("Still no URL after refresh")
                return None
            
        url = self.lastInfo['url']
        
        # Use CMAF if available for better streaming
        if self.lastInfo.get('cmaf_edge'):
            url = url.replace('playlist.m3u8', 'playlist_sfm4s.m3u8')
            url = re.sub(r'live-.+amlst', 'live-c-fhls/amlst', url)

        return self.getWantedResolutionPlaylist(url)

    def getStatus(self) -> Status:
        """Check the current status of the stream with robust retry logic."""
        max_retries = 5
        base_delay = 1.0
        
        headers = {
            "X-Requested-With": "XMLHttpRequest",
            "User-Agent": self.headers.get("User-Agent", "Mozilla/5.0"),
            "Accept": "application/json, text/plain, */*",
            "Accept-Language": "en-US,en;q=0.9",
            "Cache-Control": "no-cache",
            "Pragma": "no-cache"
        }
        data = {
            "room_slug": self.username, 
            "bandwidth": "high"
        }

        # Try each endpoint with retries
        for endpoint_idx, endpoint in enumerate(self._backup_endpoints):
            for attempt in range(max_retries):
                try:
                    # Add random jitter to prevent thundering herd
                    if attempt > 0:
                        jitter = random.uniform(0.1, 0.5)
                        delay = base_delay * (2 ** (attempt - 1)) + jitter
                        self.logger.debug(f"Retrying after {delay:.1f}s (attempt {attempt + 1}/{max_retries})")
                        time.sleep(delay)
                    
                    # Timeout increases with attempt
                    timeout = min(15 + (attempt * 5), 45)
                    
                    response = self.session.post(
                        endpoint,
                        headers=headers,
                        data=data,
                        timeout=timeout,
                        bucket='status'
                    )
                    
                    # Handle different status codes
                    if response.status_code == 200:
                        try:
                            self.lastInfo = response.json()
                            self._last_successful_request = time.time()
                            self._request_failure_count = 0
                            
                            room_status = self.lastInfo.get("room_status", "").lower()
                            status = self._parseStatus(room_status)
                            if status == Status.PUBLIC and not self.lastInfo.get('url'):
                                self.logger.debug(f"Public status but no URL for {self.username}")
                                status = Status.RESTRICTED
                                
                            self.ratelimit = False
                            return status
                            
                        except (ValueError, KeyError) as e:
                            self.logger.warning(f"JSON parse error for {self.username}: {e}")
                            if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                                return Status.RATELIMIT
                            continue
                            
                    elif response.status_code == 404:
                        self.logger.info(f"User {self.username} not found (404)")
                        return Status.NOTEXIST
                        
                    elif response.status_code == 429:
                        self.logger.warning(f"Rate limited (429) for {self.username}")
                        self.ratelimit = True
                        return Status.RATELIMIT
                        
                    elif response.status_code in (403, 503):
                        # Likely cloudflare or temporary block
                        self.logger.warning(f"Blocked ({response.status_code}) for {self.username}")
                        if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                            return Status.RATELIMIT
                        continue
                        
                    elif response.status_code >= 500:
                        # Server error - retry
                        self.logger.warning(f"Server error {response.status_code} for {self.username}")
                        if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                            return Status.RATELIMIT
                        continue
                        
                    else:
                        self.logger.warning(f"Unexpected status {response.status_code} for {self.username}")
                        if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                            return Status.RATELIMIT
                        continue
                        
                except (requests.exceptions.Timeout, requests.exceptions.ConnectTimeout) as e:
                    self.logger.warning(f"Timeout checking {self.username}: {e}")
                    if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                        return Status.RATELIMIT
                    continue
                    
                except (requests.exceptions.ConnectionError, requests.exceptions.ChunkedEncodingError) as e:
                    self.logger.warning(f"Connection error for {self.username}: {e}")
                    if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                        return Status.RATELIMIT
                    continue
                    
                except requests.exceptions.RequestException as e:
                    self.logger.warning(f"Request error for {self.username}: {e}")
                    if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                        return Status.RATELIMIT
                    continue
                    
                except Exception as e:
                    self.logger.error(f"Unexpected error checking {self.username}: {e}")
                    if attempt == max_retries - 1 and endpoint_idx == len(self._backup_endpoints) - 1:
                        return Status.RATELIMIT
                    continue
        
        # All endpoints and retries failed
        self._request_failure_count += 1
        self.logger.error(f"All endpoints failed for {self.username} (failures: {self._request_failure_count})")
        self.ratelimit = True
        return Status.RATELIMIT

    @staticmethod
    def _parseStatus(room_status: str) -> Status:
        """Parse room status string into Status enum."""
        if room_status == "public":
            return Status.PUBLIC
        elif room_status in ("private", "hidden"):
            return Status.PRIVATE
        elif room_status == "offline":
            return Status.OFFLINE
        else:
            return Status.OFFLINE

    @classmethod
    def getStatusBulk(cls, streamers: Set['Chaturbate']) -> None:
        """Bulk status update using the affiliates API with resilient error handling."""
        if not streamers:
            return
            
        session = requests.Session()
        session.headers.update(cls.headers)
        
        # Try multiple endpoints for bulk API
        endpoints = [
            "https://chaturbate.com/affiliates/api/onlinerooms/?format=json&wm=DkfRj",
            "https://en.chaturbate.com/affiliates/api/onlinerooms/?format=json&wm=DkfRj"
        ]
        
        for endpoint in endpoints:
            try:
                r = session.get(endpoint, timeout=30)
                if r.status_code != 200:
                    continue
                    
                try:
                    data = r.json()
                    if not isinstance(data, list):
                        continue
                except (requests.exceptions.JSONDecodeError, ValueError):
                    continue

                data_map = {str(model.get('username', '')).lower(): model for model in data if model.get('username')}
                successful_updates = 0

                for streamer in streamers:
                    try:
                        model_data = data_map.get(streamer.username.lower())
                        if not model_data:
                            # Only set offline if we haven't seen them recently
                            if streamer.sc not in (Status.PUBLIC, Status.PRIVATE):
                                streamer.setStatus(Status.OFFLINE)
                            continue
                            
                        # Update gender and country if available
                        if model_data.get('gender'):
                            streamer.gender = cls._GENDER_MAP.get(model_data['gender'], Gender.UNKNOWN)
                        if model_data.get('country'):
                            streamer.country = model_data.get('country', '').upper()
                            
                        status = cls._parseStatus(model_data.get('current_show', ''))
                        
                        # For public status, verify with individual check to get stream URL
                        if status == Status.PUBLIC:
                            if streamer.sc in (Status.PUBLIC, Status.RESTRICTED):
                                # Already verified, skip individual check
                                continue
                            # Need individual check to get stream URL
                            status = streamer.getStatus()
                            
                        streamer.setStatus(status)
                        successful_updates += 1
                        
                    except Exception as e:
                        # Continue processing other streamers even if one fails
                        continue
                        
                # If we got some updates, consider it successful
                if successful_updates > 0:
                    return
                    
            except Exception as e:
                # Try next endpoint
                continue
        
        # All bulk endpoints failed - streamers will fall back to individual polling

    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False
