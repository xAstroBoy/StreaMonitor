import re
import time
import requests
import base64
import hashlib
import random
import itertools

from streamonitor.bot import Bot
from streamonitor.downloaders.hls import getVideoNativeHLS
from streamonitor.enums import Status
from streamonitor.utils.CloudflareDetection import looks_like_cf_html


class StripChat(Bot):
    site = 'StripChat'
    siteslug = 'SC'
    isMobileBroadcast = False

    _static_data = None
    _main_js_data = None
    _doppio_js_data = None
    _native_js_data = None
    _pkey = None
    _mouflon_keys: dict = None
    _cached_keys: dict = None

    def __init__(self, username):
        if StripChat._static_data is None:
            StripChat._static_data = {}
            try:
                self.getInitialData()
            except Exception as e:
                StripChat._static_data = None
                raise e
        while StripChat._static_data == {}:
            time.sleep(1)
        super().__init__(username)
        # from static data, set pkey
        self.pkey = StripChat._pkey
        self.vr = False
        self.url = self.getWebsiteURL()
        self.getVideo = lambda _, url, filename: getVideoNativeHLS(self, url, filename, None)

    def get_site_color(self):
        """Return the color scheme for this site"""
        return ("green", [])

    @classmethod
    def getInitialData(cls):
        try:
            r = requests.get('https://hu.stripchat.com/api/front/v3/config/static', headers=cls.headers)
            if r.status_code != 200:
                raise Exception("Failed to fetch static data from StripChat")
            StripChat._static_data = r.json().get('static')
            if not StripChat._static_data:
                raise Exception("Static data is empty")

            mmp_origin = StripChat._static_data['features']['MMPExternalSourceOrigin']
            mmp_version = StripChat._static_data['featuresV2']['playerModuleExternalLoading']['mmpVersion']
            mmp_base = f"{mmp_origin}/v{mmp_version}"

            r = requests.get(f"{mmp_base}/main.js", headers=cls.headers)
            if r.status_code != 200:
                raise Exception("Failed to fetch main.js from StripChat")
            StripChat._main_js_data = r.content.decode('utf-8')

            doppio_js_matches = re.findall('require[(]"./(Doppio.*?[.]js)"[)]', StripChat._main_js_data)
            if not doppio_js_matches:
                raise Exception("Could not find Doppio.js filename in main.js")
            doppio_js_name = doppio_js_matches[0]

            r = requests.get(f"{mmp_base}/{doppio_js_name}", headers=cls.headers)
            if r.status_code != 200:
                raise Exception("Failed to fetch doppio.js from StripChat")
            StripChat._doppio_js_data = r.content.decode('utf-8')

            # get pkey
            try:
                native_js_matches = re.findall('require[(]"./(Native.*?[.]js)"[)]', cls._main_js_data)
                if not native_js_matches:
                    raise Exception("Could not find Native.js filename in main.js")
                native_js_name = native_js_matches[0]
                
                r = requests.get(f"{mmp_base}/{native_js_name}", headers=cls.headers)
                if r.status_code != 200:
                    raise Exception("Failed to fetch native.js")
                cls._native_js_data = r.content.decode('utf-8')
                
                # Try multiple regex patterns for pkey extraction
                pkey_patterns = [
                    r'pkey\s*:\s*"([^"]+)"',
                    r'pkey\s*=\s*"([^"]+)"',
                    r'"pkey"\s*:\s*"([^"]+)"',
                    r'pkey\s*:\s*\'([^\']+)\'',
                    r'pkey:"([^"]+)"'
                ]
                
                for pattern in pkey_patterns:
                    matches = re.findall(pattern, cls._native_js_data)
                    if matches:
                        StripChat._pkey = matches[0]
                        break
                else:
                    print("StripChat: Warning - Could not extract pkey from native.js")
                    
            except Exception as e:
                print(f"StripChat: Error extracting pkey: {e}")
                
        except Exception as e:
            print(f"StripChat: Fatal error in getInitialData: {e}")
            raise

    @classmethod
    def m3u_decoder(cls, content):
        """Decode M3U8 playlists with Mouflon encryption using optimized XOR cipher."""
        def _decode(encrypted_b64: str, key: str) -> str:
            if cls._cached_keys is None:
                cls._cached_keys = {}
            hash_bytes = cls._cached_keys.get(key)
            if hash_bytes is None:
                hash_bytes = hashlib.sha256(key.encode("utf-8")).digest()
                cls._cached_keys[key] = hash_bytes
            
            encrypted_data = base64.b64decode(encrypted_b64 + "==")
            # Use itertools.cycle for efficient XOR operation
            decrypted = bytes(a ^ b for a, b in zip(encrypted_data, itertools.cycle(hash_bytes)))
            return decrypted.decode("utf-8")

        _, pkey = cls._getMouflonFromM3U(content)
        if not pkey:
            return content

        decoded = []
        lines = content.splitlines()
        for idx, line in enumerate(lines):
            if line.startswith("#EXT-X-MOUFLON:FILE:"):
                dec = _decode(line[20:], cls.getMouflonDecKey(pkey))
                # Replace media.mp4 in next line with decoded filename
                if idx + 1 < len(lines):
                    decoded.append(lines[idx + 1].replace("media.mp4", dec))
            elif not (idx > 0 and lines[idx - 1].startswith("#EXT-X-MOUFLON:FILE:")):
                decoded.append(line)
        
        return "\n".join(decoded)

    @classmethod
    def getMouflonDecKey(cls, pkey):
        """Get or fetch the decryption key for a given pkey from cached JS data."""
        if cls._mouflon_keys is None:
            cls._mouflon_keys = {}

        if pkey in cls._mouflon_keys:
            return cls._mouflon_keys[pkey]

        key_matches = re.findall(f'"{pkey}:(.*?)"', cls._doppio_js_data)
        if not key_matches:
            raise Exception(f"Could not find decryption key for pkey: {pkey}")
        key = key_matches[0]
        cls._mouflon_keys[pkey] = key
        return key

    @staticmethod
    def _getMouflonFromM3U(m3u8_doc):
        """Extract Mouflon encryption parameters from M3U8 playlist."""
        if '#EXT-X-MOUFLON:' in m3u8_doc:
            _mouflon_start = m3u8_doc.find('#EXT-X-MOUFLON:')
            if _mouflon_start >= 0:
                _mouflon = m3u8_doc[_mouflon_start:m3u8_doc.find('\n', _mouflon_start)].strip().split(':')
                if len(_mouflon) >= 4:
                    psch = _mouflon[2]
                    pkey = _mouflon[3]
                    return psch, pkey
        return None, None

    def getWebsiteURL(self):
        return f"https://stripchat.com/{self.username}"

    def getVideoUrl(self):
        return self.getWantedResolutionPlaylist(None)

    @staticmethod
    def uniq():
        """Generate a random unique string for API requests."""
        chars = ''.join(chr(i) for i in range(ord('a'), ord('z') + 1))
        chars += ''.join(chr(i) for i in range(ord('0'), ord('9') + 1))
        return ''.join(random.choice(chars) for _ in range(16))

    @staticmethod
    def normalizeInfo(raw: dict) -> dict:
        """
        Normalize JSON so lastInfo is always a dict.
        Keep top-level where possible; flatten only obvious wrappers like `item` or single-element lists.
        """
        if not raw:
            return {}
        # If it's a list, use first element (common wrapping)
        if isinstance(raw, list):
            return raw[0] if raw else {}
        # If it's a dict with 'item' containing the real object
        if isinstance(raw, dict) and "item" in raw and isinstance(raw["item"], dict):
            return raw["item"]
        # If it's a dict with a single top wrapper like {"data": {...}} try common keys but avoid dropping cam/user
        if isinstance(raw, dict) and len(raw) == 1:
            sole_key = next(iter(raw))
            if sole_key in ("data", "result", "response") and isinstance(raw[sole_key], dict):
                return raw[sole_key]
        # Otherwise return as-is (we'll handle nested shapes in getters)
        return raw

    def _get_by_path(self, data: dict, path: list):
        """
        Safely get nested value by path list from dict-like structures.
        Returns None if any step is missing or type mismatch.
        """
        cur = data
        for p in path:
            if isinstance(cur, dict) and p in cur:
                cur = cur[p]
            else:
                return None
        return cur

    def _recursive_find(self, data, key):
        """
        Depth-first search for first occurrence of `key` in nested dict/list objects.
        Returns the value if found, otherwise None.
        """
        if isinstance(data, dict):
            if key in data:
                return data[key]
            for v in data.values():
                found = self._recursive_find(v, key)
                if found is not None:
                    return found
        elif isinstance(data, list):
            for item in data:
                found = self._recursive_find(item, key)
                if found is not None:
                    return found
        return None

    def _first_in_paths(self, paths: list):
        """
        Try a list of paths (each path is a list of keys). Return first non-None value.
        """
        for p in paths:
            val = self._get_by_path(self.lastInfo, p)
            if val is not None:
                return val
        return None

    def getStreamName(self) -> str:
        """
        Robust streamName detector: checks multiple common locations and finally
        performs a recursive search for the key if not found.
        Raises KeyError if nothing found.
        """
        if not self.lastInfo:
            raise KeyError("lastInfo is empty, call getStatus() first")

        # Common paths to check in order
        paths = [
            ["streamName"],
            ["cam", "streamName"],
            ["user", "streamName"],
            ["user", "user", "streamName"],
            ["model", "streamName"],
            ["user", "user", "userStreamName"],
            ["cam", "userStreamName"],
        ]
        val = self._first_in_paths(paths)
        if val:
            return str(val)

        # Last resort: recursive find
        val = self._recursive_find(self.lastInfo, "streamName")
        if val:
            return str(val)

        raise KeyError(f"No streamName in lastInfo: keys={list(self.lastInfo.keys())}")

    def getStatusField(self):
        """
        Robust status detector. Tries known paths and falls back to recursive search.
        Returns the raw status value or None.
        """
        if not self.lastInfo:
            return None

        # Explicit candidate paths (ordered)
        paths = [
            ["status"],
            ["cam", "streamStatus"],
            ["cam", "status"],
            ["model", "status"],
            ["user", "status"],
            ["user", "user", "status"],
            ["user", "user", "state"],
            ["user", "user", "broadcastStatus"],
        ]
        status = self._first_in_paths(paths)
        if status is not None:
            return status

        # Recursive fallback — but prefer strings that match expected status tokens
        found = self._recursive_find(self.lastInfo, "status")
        if isinstance(found, str):
            return found
        return None

    def getIsLive(self) -> bool:
        """
        Robust isLive detector. Checks multiple locations and returns boolean.
        """
        if not self.lastInfo:
            return False

        # Try direct root flag
        val = self._get_by_path(self.lastInfo, ["isLive"])
        if val is not None:
            return bool(val)

        # Common locations
        paths = [
            ["cam", "isCamActive"],
            ["cam", "isCamAvailable"],
            ["cam", "isLive"],
            ["model", "isLive"],
            ["user", "isLive"],
            ["user", "user", "isLive"],
            ["user", "user", "isCamActive"],
            ["broadcastSettings", "isLive"],
            ["cam", "broadcastSettings", "isCamActive"],
            ["cam", "broadcastSettings", "isLive"],
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            return bool(val)

        # Recursive fallback for any key named isLive or isCamActive
        for k in ("isLive", "isCamActive", "isCamAvailable"):
            found = self._recursive_find(self.lastInfo, k)
            if found is not None:
                return bool(found)

        return False

    def getIsMobile(self) -> bool:
        """
        Robust isMobile detector. Checks multiple locations and returns boolean.
        """
        if not self.lastInfo:
            return False

        # Direct
        val = self._get_by_path(self.lastInfo, ["isMobile"])
        if val is not None:
            return bool(val)

        # Common paths
        paths = [
            ["model", "isMobile"],
            ["user", "isMobile"],
            ["user", "user", "isMobile"],
            ["broadcastSettings", "isMobile"],
            ["cam", "broadcastSettings", "isMobile"],
            ["cam", "isMobile"],
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            return bool(val)

        # Recursive fallback
        found = self._recursive_find(self.lastInfo, "isMobile")
        if found is not None:
            return bool(found)

        return False

    def getIsGeoBanned(self) -> bool:
        """Check if user is geo-banned from viewing this model."""
        if not self.lastInfo:
            return False

        paths = [
            ["isGeoBanned"],
            ["user", "isGeoBanned"],
            ["user", "user", "isGeoBanned"],
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            return bool(val)

        found = self._recursive_find(self.lastInfo, "isGeoBanned")
        return bool(found) if found is not None else False

    def getIsDeleted(self) -> bool:
        """Check if the model account has been deleted."""
        if not self.lastInfo:
            return False

        # Try common paths where isDeleted might be found
        paths = [
            ["isDeleted"],
            ["user", "isDeleted"],
            ["user", "user", "isDeleted"],
            ["model", "isDeleted"],
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            return bool(val)

        # Recursive fallback
        found = self._recursive_find(self.lastInfo, "isDeleted")
        return bool(found) if found is not None else False

    def getStatus(self):
        """Check the current status of the model's stream."""
        url = f'https://stripchat.com/api/front/v2/models/username/{self.username}/cam?uniq={StripChat.uniq()}'
        r = self.session.get(url, headers=self.headers, bucket='api')

        ct = (r.headers.get("content-type") or "").lower()
        body = r.text or ""

        # Handle HTTP errors
        if r.status_code == 404:
            return Status.NOTEXIST
        if r.status_code == 403:
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge (403) for {self.username}')
                return Status.CLOUDFLARE
            return Status.RESTRICTED
        if r.status_code == 429:
            self.logger.error(f'Rate limited (429) for {self.username}')
            return Status.RATELIMIT
        if r.status_code >= 500:
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge ({r.status_code}) for {self.username}')
                return Status.CLOUDFLARE
            self.logger.error(f'Server error {r.status_code} for {self.username}')
            return Status.UNKNOWN

        # Validate JSON response
        if "application/json" not in ct or not body.strip():
            self.logger.warning(f'Non-JSON response for {self.username}')
            return Status.UNKNOWN

        try:
            raw = r.json()
        except Exception as e:
            self.logger.error(f'Failed to parse JSON for {self.username}: {e}')
            return Status.UNKNOWN

        # Normalize and store info
        self.lastInfo = self.normalizeInfo(raw)
        self.isMobileBroadcast = self.getIsMobile()

        # Check for geo-ban
        if self.getIsGeoBanned():
            return Status.RESTRICTED

        # Check if model account has been deleted
        if self.getIsDeleted():
            self.logger.warning(f'⚠️ Model account {self.username} has been deleted - this model will be auto-deregistered')
            return Status.DELETED

        # Determine status
        status = self.getStatusField()
        is_live = self.getIsLive()

        if status == "public" and is_live:
            return Status.PUBLIC
        if status in ["private", "groupShow", "p2p", "virtualPrivate", "p2pVoice"]:
            return Status.PRIVATE
        if not is_live or status in ["off", "idle"]:
            return Status.OFFLINE

        self.logger.warning(f"Unknown status '{status}' for {self.username}")
        return Status.UNKNOWN

    def isMobile(self):
        """Check if the current broadcast is from a mobile device."""
        return self.isMobileBroadcast

    def getPlaylistVariants(self, url):
        """Get available video quality variants from the HLS master playlist."""
        try:
            stream_id = self.getStreamName()
            self.logger.debug(f"Stream ID: {stream_id}")
        except Exception as e:
            self.logger.error(f"Failed to get stream name: {e}")
            raise

        host = f'doppiocdn.{random.choice(["org", "com", "net"])}'

        # Use random host selection for load balancing
        host = f'doppiocdn.{random.choice(["org", "com", "net"])}'
        url = "https://edge-hls.{host}/hls/{id}{vr}/master/{id}{vr}{auto}.m3u8?psch=v1&pkey={pkey}".format(
            host=host,
            id=stream_id,
            vr='_vr' if self.vr else '',
            auto='_auto' if not self.vr else '',
            pkey=self.pkey,
        )
        
        
        self.logger.debug(f"Requesting playlist URL: {url}")
        
        try:
            result = requests.get(url, headers=self.headers, cookies=self.cookies)
            if result.status_code != 200:
                self.logger.error(f"Failed to fetch playlist: HTTP {result.status_code}")
                raise Exception(f"HTTP {result.status_code} error fetching playlist")
            
            m3u8_doc = result.content.decode("utf-8")
            self.logger.debug(f"M3U8 document length: {len(m3u8_doc)}")
            
            psch, pkey = StripChat._getMouflonFromM3U(m3u8_doc)
            self.logger.debug(f"Extracted from M3U8 - psch: {psch}, pkey: {pkey}")
            
            variants = super().getPlaylistVariants(m3u_data=m3u8_doc)
            self.logger.debug(f"Found {len(variants)} variants")
            
            # Add pkey parameter to each variant URL
            if pkey:
                for variant in variants:
                    variant['url'] = f'{variant["url"]}{"&" if "?" in variant["url"] else "?"}'
            
            return variants
            
        except Exception as e:
            self.logger.error(f"Error in getPlaylistVariants: {e}")
            raise


Bot.loaded_sites.add(StripChat)