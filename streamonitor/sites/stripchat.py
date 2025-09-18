
import re
import time
import requests
import base64
import hashlib

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
        self.vr = False
        self.url = self.getWebsiteURL()
        self.getVideo = lambda _, url, filename: getVideoNativeHLS(self, url, filename, StripChat.m3u_decoder)

    @classmethod
    def getInitialData(cls):
        r = requests.get('https://hu.stripchat.com/api/front/v3/config/static', headers=cls.headers)
        if r.status_code != 200:
            raise Exception("Failed to fetch static data from StripChat")
        StripChat._static_data = r.json().get('static')

        mmp_origin = StripChat._static_data['features']['MMPExternalSourceOrigin']
        mmp_version = StripChat._static_data['featuresV2']['playerModuleExternalLoading']['mmpVersion']
        mmp_base = f"{mmp_origin}/v{mmp_version}"

        r = requests.get(f"{mmp_base}/main.js", headers=cls.headers)
        if r.status_code != 200:
            raise Exception("Failed to fetch main.js from StripChat")
        StripChat._main_js_data = r.content.decode('utf-8')

        doppio_js_name = re.findall('require[(]"./(Doppio.*?[.]js)"[)]', StripChat._main_js_data)[0]

        r = requests.get(f"{mmp_base}/{doppio_js_name}", headers=cls.headers)
        if r.status_code != 200:
            raise Exception("Failed to fetch doppio.js from StripChat")
        StripChat._doppio_js_data = r.content.decode('utf-8')

    @classmethod
    def m3u_decoder(cls, content):
        def _decode(encrypted_b64: str, key: str) -> str:
            if cls._cached_keys is None:
                cls._cached_keys = {}
            if key not in cls._cached_keys:
                cls._cached_keys[key] = hashlib.sha256(key.encode("utf-8")).digest()
            hash_bytes = cls._cached_keys[key]
            hash_len = len(hash_bytes)

            encrypted_data = base64.b64decode(encrypted_b64 + "==")

            decrypted_bytes = bytearray()
            for i, cipher_byte in enumerate(encrypted_data):
                key_byte = hash_bytes[i % hash_len]
                decrypted_byte = cipher_byte ^ key_byte
                decrypted_bytes.append(decrypted_byte)

            plaintext = decrypted_bytes.decode("utf-8")
            return plaintext

        _, pkey = StripChat._getMouflonFromM3U(content)

        decoded = []
        lines = content.splitlines()
        for idx, line in enumerate(lines):
            if line.startswith("#EXT-X-MOUFLON:FILE:"):
                dec = _decode(line[20:], cls.getMouflonDecKey(pkey))
                decoded.append(lines[idx + 1].replace("media.mp4", dec))
            else:
                decoded.append(line)
        return "\n".join(decoded)

    @classmethod
    def getMouflonDecKey(cls, pkey):
        if not cls._mouflon_keys:
            cls._mouflon_keys = {}

        if pkey in cls._mouflon_keys:
            return cls._mouflon_keys[pkey]

        key = re.findall(f'"{pkey}:(.*?)"', cls._doppio_js_data)[0]
        cls._mouflon_keys[pkey] = key
        return key

    @staticmethod
    def _getMouflonFromM3U(m3u8_doc):
        if '#EXT-X-MOUFLON:' in m3u8_doc:
            _mouflon_start = m3u8_doc.find('#EXT-X-MOUFLON:')
            if _mouflon_start > 0:
                _mouflon = m3u8_doc[_mouflon_start:m3u8_doc.find('\n', _mouflon_start)].strip().split(':')
                psch = _mouflon[2]
                pkey = _mouflon[3]
                return psch, pkey
        return None, None

    def getWebsiteURL(self):
        return "https://stripchat.com/" + self.username

    def getVideoUrl(self):
        return self.getWantedResolutionPlaylist(None)

    def getPlaylistVariants(self, url):
        url = "https://edge-hls.{host}/hls/{id}{vr}/master/{id}{vr}{auto}.m3u8".format(
                host='doppiocdn.com',
                id=self.lastInfo["cam"]["streamName"],
                vr='_vr' if self.vr else '',
                auto='_auto' if not self.vr else ''
            )
        result = requests.get(url, headers=self.headers, cookies=self.cookies)
        m3u8_doc = result.content.decode("utf-8")
        psch, pkey = StripChat._getMouflonFromM3U(m3u8_doc)
        variants = super().getPlaylistVariants(m3u_data=m3u8_doc)
        return [variant | {'url': f'{variant["url"]}{"&" if "?" in variant["url"] else "?"}psch={psch}&pkey={pkey}'}
                for variant in variants]





    def getStatus(self):
        url = 'https://vr.stripchat.com/api/vr/v2/models/username/' + self.username
        r = self.session.get(url, headers=self.headers)

        ct = (r.headers.get("content-type") or "").lower()
        body = r.text or ""

        # Map obvious non-success first
        if r.status_code == 404:
            return Status.NOTEXIST
        if r.status_code == 403:
            # Could be geo/restricted or CF. Prefer CF if HTML challenge detected.
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge (403) for {self.username}')
                return Status.CLOUDFLARE
            return Status.RESTRICTED
        if r.status_code == 429:
            self.logger.error(f'Rate limited (429) for {self.username}')
            return Status.RATELIMIT
        if r.status_code >= 500:
            # CF sometimes returns 503 with the challenge page
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge ({r.status_code}) for {self.username}')
                return Status.CLOUDFLARE
            self.logger.error(f'Server error {r.status_code} for {self.username}')
            return Status.UNKNOWN

        # If it's not JSON, don't try to json() it
        if "application/json" not in ct:
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge (HTML) for {self.username}')
                return Status.CLOUDFLARE
            # Unexpected content-type
            snippet = body[:200].replace("\n", " ")
            self.logger.error(f'Non-JSON reply ({ct}) for {self.username}. Snippet: {snippet}')
            return Status.UNKNOWN

        # Guard against empty/whitespace body
        if not body.strip():
            self.logger.error(f'Empty JSON body for {self.username} (status {r.status_code})')
            return Status.UNKNOWN

        # Safe to parse JSON
        try:
            self.lastInfo = r.json()
        except Exception as e:
            snippet = body[:200].replace("\n", " ")
            self.logger.error(
                f'Failed to decode JSON for {self.username}. Status {r.status_code}. Snippet: {snippet}'
            )
            # Heuristics again
            if looks_like_cf_html(body):
                return Status.CLOUDFLARE
            if r.status_code == 429:
                return Status.RATELIMIT
            if r.status_code == 403:
                return Status.RESTRICTED
            if r.status_code == 404:
                return Status.NOTEXIST
            return Status.UNKNOWN

        # Normal status mapping
        self.isMobileBroadcast = (
            self.lastInfo.get("broadcastSettings", {}).get("isMobile")
            or self.lastInfo.get("model", {}).get("isMobile")
            or False
        )

        model = self.lastInfo.get("model", {}) or {}
        cam = self.lastInfo.get("cam", {}) or {}

        # Prefer cam.streamStatus if present, otherwise fall back to model.status
        status = (
            cam.get("streamStatus")
            or model.get("status")
            or self.lastInfo.get("status")  # futureproof in case status moves root
        )

        if status == "public" and self.lastInfo.get("isCamAvailable") and cam.get("isCamActive"):
            return Status.PUBLIC

        if status in ["private", "groupShow", "p2p", "virtualPrivate", "p2pVoice"]:
            return Status.PRIVATE

        if status in ["off", "idle"]:
            return Status.OFFLINE

        self.logger.warning(
            f"Unknown status: {status} "
            f"isCamAvailable={self.lastInfo.get('isCamAvailable')} "
            f"isCamActive={cam.get('isCamActive')}"
        )
        return Status.UNKNOWN



    def isMobile(self):
        return self.isMobileBroadcast

Bot.loaded_sites.add(StripChat)